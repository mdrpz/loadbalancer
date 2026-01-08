#include "core/load_balancer.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include "config/config.h"
#include "core/backend_node.h"
#include "core/http_data_forwarder.h"
#include "core/splice_forwarder.h"
#include "health/health_checker.h"
#include "logging/logger.h"
#include "metrics/metrics.h"
#include "tls/tls_context.h"

namespace lb::core {

LoadBalancer::LoadBalancer()
    : max_global_connections_(1000), max_connections_per_backend_(100),
      backpressure_timeout_ms_(10000), connection_timeout_seconds_(5), config_manager_(nullptr),
      mode_("tcp") {
    reactor_ = std::make_unique<net::EpollReactor>();
    backend_pool_ = std::make_unique<BackendPool>();
    health_checker_ = std::make_unique<lb::health::HealthChecker>();

    connection_manager_ = std::make_unique<ConnectionManager>(
        connections_, backend_connections_, connection_times_, backpressure_start_times_,
        client_retry_counts_, backend_to_client_map_, *reactor_);

    backpressure_manager_ =
        std::make_unique<BackpressureManager>(backpressure_start_times_, backpressure_timeout_ms_);

    data_forwarder_ = std::make_unique<DataForwarder>(
        *reactor_, [this](int fd) { backpressure_manager_->start_tracking(fd); },
        [this](int fd) { backpressure_manager_->clear_tracking(fd); },
        [this](int fd) { connection_manager_->close_connection(fd); });

    retry_handler_ = std::make_unique<RetryHandler>(
        [this](std::unique_ptr<net::Connection> conn, int retry_count) {
            backend_connector_->connect(std::move(conn), retry_count);
        });

    event_handlers_ = std::make_unique<EventHandlers>(
        connections_, backend_connections_, connection_times_, backend_to_client_map_,
        client_retry_counts_, *reactor_, connection_timeout_seconds_,
        [this](int fd) { return connection_manager_->get_connection(fd); },
        [this](int fd) {
            backpressure_manager_->check_timeout(
                fd, [this](int f) { connection_manager_->close_connection(f); });
        },
        [this](int fd) {
            auto* conn = connection_manager_->get_connection(fd);
            if (conn && conn->is_tls() && tls_context_) {
                SSL* ssl = conn->ssl();
                if (ssl) {
                    conn->set_ssl(nullptr);
                    tls_context_->destroy_ssl(ssl);
                }
            }
            if (splice_forwarder_) {
                splice_forwarder_->cleanup(fd);
            }
            connection_manager_->close_connection(fd);
        },
        [this](int fd) { connection_manager_->close_backend_connection_only(fd); },
        [this](net::Connection* from, net::Connection* to) {
            if (mode_ == "http" && http_data_forwarder_) {
                http_data_forwarder_->forward(from, to);
            } else if (use_splice_ && splice_forwarder_) {
                splice_forwarder_->forward(from, to);
            } else {
                data_forwarder_->forward(from, to);
            }
        },
        [this](int fd) { backpressure_manager_->clear_tracking(fd); },
        [this](std::unique_ptr<net::Connection> conn, int retry_count) {
            retry_handler_->retry(std::move(conn), retry_count);
        },
        [this](int fd) {
            auto it = connections_.find(fd);
            if (it != connections_.end() && it->second) {
                auto client_conn = std::move(it->second);
                connections_.erase(it);
                backend_connector_->connect(std::move(client_conn), 0);
            }
        });

    backend_connector_ = std::make_unique<BackendConnector>(
        connections_, backend_connections_, connection_times_, backend_to_client_map_,
        client_retry_counts_, *backend_pool_, *reactor_, max_connections_per_backend_,
        [this](int fd, net::EventType type) { event_handlers_->handle_client_event(fd, type); },
        [this](int fd, net::EventType type) { event_handlers_->handle_backend_event(fd, type); },
        [this](std::unique_ptr<net::Connection> conn, int retry_count) {
            retry_handler_->retry(std::move(conn), retry_count);
        });
}

LoadBalancer::~LoadBalancer() {
    if (health_checker_)
        health_checker_->stop();

    if (tls_context_) {
        for (auto& [fd, conn] : connections_) {
            if (conn && conn->is_tls()) {
                tls_context_->destroy_ssl(conn->ssl());
            }
        }
    }

    for (auto& [fd, conn] : connections_) {
        if (conn)
            conn->close();
    }
    connections_.clear();
}

bool LoadBalancer::initialize(const std::string& listen_host, uint16_t listen_port) {
    listener_ = std::make_unique<net::TcpListener>();
    if (!listener_->bind(listen_host, listen_port)) {
        lb::logging::Logger::instance().error("Failed to bind to " + listen_host + ":" +
                                              std::to_string(listen_port));
        return false;
    }
    if (!listener_->listen()) {
        lb::logging::Logger::instance().error("Failed to listen on " + listen_host + ":" +
                                              std::to_string(listen_port));
        return false;
    }
    int listener_fd = listener_->fd();
    if (!reactor_->add_fd(listener_fd, EPOLLIN, [this](int fd, net::EventType type) {
            (void)fd;
            (void)type;
            handle_accept();
        })) {
        lb::logging::Logger::instance().error("Failed to register listener with reactor");
        return false;
    }
    if (health_checker_)
        health_checker_->start();

    lb::logging::Logger::instance().info("Load balancer initialized on " + listen_host + ":" +
                                         std::to_string(listen_port));
    return true;
}

void LoadBalancer::set_config_manager(lb::config::ConfigManager* config_manager) {
    config_manager_ = config_manager;

    if (reactor_ && config_manager_)
        reactor_->set_periodic_callback(
            [this]() {
                if (config_manager_ && config_manager_->check_and_reload()) {
                    auto new_config = config_manager_->get_config();
                    if (new_config)
                        apply_config(new_config);
                }
                cleanup_drained_backends();
                if (pool_manager_) {
                    pool_manager_->evict_expired();
                }
            },
            5000);
}

void LoadBalancer::run() {
    if (reactor_)
        reactor_->run();
}

void LoadBalancer::stop() {
    if (health_checker_)
        health_checker_->stop();
    if (reactor_)
        reactor_->stop();
}

bool LoadBalancer::initialize_from_config(const std::shared_ptr<const lb::config::Config>& config) {
    if (!config)
        return false;
    if (!initialize(config->listen_host, config->listen_port))
        return false;
    max_global_connections_ = config->max_global_connections;
    max_connections_per_backend_ = config->max_connections_per_backend;
    backpressure_timeout_ms_ = config->backpressure_timeout_ms;
    mode_ = config->mode.empty() ? "tcp" : config->mode;

    backpressure_manager_ =
        std::make_unique<BackpressureManager>(backpressure_start_times_, backpressure_timeout_ms_);

    if (mode_ == "http") {
        http_data_forwarder_ = std::make_unique<HttpDataForwarder>(
            *reactor_, [this](int fd) { backpressure_manager_->start_tracking(fd); },
            [this](int fd) { backpressure_manager_->clear_tracking(fd); },
            [this](int fd) { connection_manager_->close_connection(fd); },
            [this](int fd) {
                auto* conn = connection_manager_->get_connection(fd);
                if (conn) {
                    struct sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
                        char ip_str[INET_ADDRSTRLEN];
                        if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN) !=
                            nullptr) {
                            return std::string(ip_str);
                        }
                    }
                }
                return std::string();
            },
            tls_context_ && tls_context_->is_initialized());
        lb::logging::Logger::instance().info("HTTP mode enabled");
    }

    use_splice_ = config->use_splice && mode_ == "tcp";
    if (use_splice_) {
        if (SpliceForwarder::is_available()) {
            splice_forwarder_ = std::make_unique<SpliceForwarder>(
                *reactor_, [this](int fd) { backpressure_manager_->start_tracking(fd); },
                [this](int fd) { backpressure_manager_->clear_tracking(fd); },
                [this](int fd) { connection_manager_->close_connection(fd); });
            event_handlers_->set_splice_mode(true);
            lb::logging::Logger::instance().info("Zero-copy splice mode enabled");
        } else {
            lb::logging::Logger::instance().warn(
                "Splice requested but not available on this system, using buffer copy");
            use_splice_ = false;
        }
    }

    if (config->tls_enabled) {
        tls_context_ = std::make_unique<lb::tls::TlsContext>();
        if (!tls_context_->initialize()) {
            lb::logging::Logger::instance().error("Failed to initialize TLS context");
            return false;
        }
        if (!tls_context_->load_certificate(config->tls_cert_path, config->tls_key_path)) {
            lb::logging::Logger::instance().error("Failed to load TLS certificate/key");
            return false;
        }
        lb::logging::Logger::instance().info("TLS enabled with certificate: " +
                                             config->tls_cert_path);
    }

    for (const auto& backend_cfg : config->backends) {
        add_backend(backend_cfg.host, backend_cfg.port);
    }

    pool_enabled_ = config->connection_pool_enabled;
    if (pool_enabled_) {
        PoolConfig pool_config;
        pool_config.min_connections = config->pool_min_connections;
        pool_config.max_connections = config->pool_max_connections;
        pool_config.max_idle_time_ms = config->pool_idle_timeout_ms;
        pool_config.connect_timeout_ms = config->pool_connect_timeout_ms;
        pool_manager_ = std::make_unique<ConnectionPoolManager>(pool_config);
        backend_connector_->set_pool_manager(pool_manager_.get());

        connection_manager_->set_pool_release_callback(
            [this](int backend_fd, std::unique_ptr<net::Connection> conn) -> bool {
                if (!pool_manager_ || !conn)
                    return false;
                auto info = backend_connector_->get_backend_info(backend_fd);
                if (info.host.empty())
                    return false;
                conn->clear_buffers();
                conn->set_state(net::ConnectionState::ESTABLISHED);
                pool_manager_->release(info.host, info.port, std::move(conn));
                return true;
            });

        lb::logging::Logger::instance().info("Connection pooling enabled (max " +
                                             std::to_string(config->pool_max_connections) +
                                             " per backend)");
    }

    return true;
}

void LoadBalancer::apply_config(const std::shared_ptr<const lb::config::Config>& config) {
    if (!config)
        return;

    lb::logging::Logger::instance().info("Applying configuration reload");

    max_global_connections_ = config->max_global_connections;
    max_connections_per_backend_ = config->max_connections_per_backend;
    backpressure_timeout_ms_ = config->backpressure_timeout_ms;

    backpressure_manager_ =
        std::make_unique<BackpressureManager>(backpressure_start_times_, backpressure_timeout_ms_);

    auto current_backends = backend_pool_->get_all_backends();

    std::set<std::pair<std::string, uint16_t>> new_backend_keys;
    for (const auto& backend_cfg : config->backends) {
        new_backend_keys.insert({backend_cfg.host, backend_cfg.port});
    }

    for (const auto& backend : current_backends) {
        std::pair<std::string, uint16_t> key = {backend->host(), backend->port()};
        if (new_backend_keys.find(key) == new_backend_keys.end()) {
            if (backend->state() != BackendState::DRAINING) {
                backend->set_state(BackendState::DRAINING);
                lb::logging::Logger::instance().info("Backend " + backend->host() + ":" +
                                                     std::to_string(backend->port()) +
                                                     " marked as DRAINING");
            }
        }
    }

    for (const auto& backend_cfg : config->backends) {
        auto existing = backend_pool_->find_backend(backend_cfg.host, backend_cfg.port);
        if (!existing) {
            add_backend(backend_cfg.host, backend_cfg.port);
        } else if (existing->state() == BackendState::DRAINING) {
            existing->set_state(BackendState::HEALTHY);
            lb::logging::Logger::instance().info("Backend " + backend_cfg.host + ":" +
                                                 std::to_string(backend_cfg.port) +
                                                 " restored to HEALTHY");
        }
    }
    cleanup_drained_backends();
}

void LoadBalancer::cleanup_drained_backends() {
    auto all_backends = backend_pool_->get_all_backends();
    for (const auto& backend : all_backends) {
        if (backend->state() == BackendState::DRAINING && backend->active_connections() == 0) {
            lb::logging::Logger::instance().info("Removing drained backend " + backend->host() +
                                                 ":" + std::to_string(backend->port()));
            backend_pool_->remove_backend(backend->host(), backend->port());
            if (health_checker_)
                health_checker_->remove_backend(backend);
        }
    }
}

void LoadBalancer::add_backend(const std::string& host, uint16_t port) {
    auto existing = backend_pool_->find_backend(host, port);
    if (existing)
        return;
    auto backend = std::make_shared<BackendNode>(host, port);
    backend_pool_->add_backend(backend);

    if (health_checker_)
        health_checker_->add_backend(backend);

    lb::logging::Logger::instance().info("Added backend " + host + ":" + std::to_string(port));
}

void LoadBalancer::handle_accept() {
    while (true) {
        int client_fd = listener_->accept();
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        size_t established_count = connection_manager_->count_established_connections();
        if (established_count >= max_global_connections_) {
            lb::metrics::Metrics::instance().increment_overload_drops();
            lb::logging::Logger::instance().warn("Connection rejected: global connection limit (" +
                                                 std::to_string(max_global_connections_) +
                                                 ") exceeded");
            ::close(client_fd);
            continue;
        }
        lb::metrics::Metrics::instance().increment_connections_total();
        lb::logging::Logger::instance().debug(
            "New connection accepted (fd=" + std::to_string(client_fd) + ")");
        auto client_conn = std::make_unique<net::Connection>(client_fd);

        if (tls_context_ && tls_context_->is_initialized()) {
            SSL* ssl = tls_context_->create_ssl(client_fd);
            if (!ssl) {
                lb::logging::Logger::instance().error(
                    "Failed to create SSL object for connection (fd=" + std::to_string(client_fd) +
                    ")");
                ::close(client_fd);
                continue;
            }
            client_conn->set_ssl(ssl);
            client_conn->set_state(net::ConnectionState::HANDSHAKE);
            lb::logging::Logger::instance().debug(
                "TLS connection created, starting handshake (fd=" + std::to_string(client_fd) +
                ")");

            int client_fd_stored = client_conn->fd();
            connections_[client_fd_stored] = std::move(client_conn);

            reactor_->add_fd(client_fd_stored, EPOLLIN | EPOLLOUT,
                             [this](int fd, net::EventType type) {
                                 event_handlers_->handle_client_event(fd, type);
                             });
        } else {
            client_conn->set_state(net::ConnectionState::ESTABLISHED);
            backend_connector_->connect(std::move(client_conn), 0);
        }
    }
}

} // namespace lb::core

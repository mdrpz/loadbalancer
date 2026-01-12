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
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "config/config.h"
#include "core/backend_node.h"
#include "core/http_data_forwarder.h"
#include "core/splice_forwarder.h"
#include "health/health_checker.h"
#include "http/http_handler.h"
#include "http/http_response.h"
#include "logging/access_logger.h"
#include "logging/logger.h"
#include "metrics/metrics.h"
#include "tls/tls_context.h"

namespace lb::core {

LoadBalancer::LoadBalancer()
    : max_global_connections_(1000), max_connections_per_backend_(100),
      backpressure_timeout_ms_(10000), connection_timeout_seconds_(5), request_timeout_ms_(0),
      config_manager_(nullptr), mode_("tcp") {
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

    if (reactor_ && config_manager_) {
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
                check_request_timeouts();
                cleanup_rate_limit_entries();
            },
            1000);
    }
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
    request_timeout_ms_ = config->request_timeout_ms;
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

        if (http_data_forwarder_ && config) {
            auto headers_to_add = config->http_request_headers_add;
            auto headers_to_remove = config->http_request_headers_remove;
            if (!headers_to_add.empty() || !headers_to_remove.empty()) {
                http_data_forwarder_->set_custom_header_modifier(
                    [headers_to_add, headers_to_remove](lb::http::HttpRequest& request) {
                        lb::http::HttpHandler::apply_custom_headers(request, headers_to_add,
                                                                    headers_to_remove);
                    });
            }
        }

        if (http_data_forwarder_) {
            http_data_forwarder_->set_access_log_callback([](int, const RequestInfo& req_info) {
                if (req_info.method.empty())
                    return;

                auto end_time =
                    req_info.response_complete &&
                            req_info.response_complete_time.time_since_epoch().count() > 0
                        ? req_info.response_complete_time
                        : std::chrono::system_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    end_time - req_info.start_time);

                lb::logging::AccessLogEntry entry;
                entry.client_ip = req_info.client_ip;
                entry.method = req_info.method;
                entry.path = req_info.path;
                entry.status_code = req_info.status_code > 0 ? req_info.status_code : 0;
                entry.bytes_sent = req_info.bytes_sent;
                entry.bytes_received = req_info.bytes_received;
                entry.latency = latency;
                entry.backend = req_info.backend;
                entry.timestamp = req_info.start_time;

                lb::logging::AccessLogger::instance().log(entry);

                if (latency.count() > 0) {
                    double latency_ms = latency.count() / 1000.0;
                    lb::metrics::Metrics::instance().record_request_latency_ms(latency_ms);
                }
            });

            connection_manager_->set_access_log_callback([this](int client_fd) -> RequestInfo* {
                return http_data_forwarder_->get_request_info(client_fd);
            });
            connection_manager_->set_clear_request_info_callback(
                [this](int client_fd) { http_data_forwarder_->clear_request_info(client_fd); });
            http_data_forwarder_->set_backend_to_client_map([this](int backend_fd) -> int {
                auto it = backend_to_client_map_.find(backend_fd);
                return (it != backend_to_client_map_.end()) ? it->second : 0;
            });
        }

        if (backend_connector_ && http_data_forwarder_) {
            backend_connector_->set_backend_selected_callback(
                [this](int client_fd, const std::string& host, uint16_t port) {
                    std::ostringstream backend_str;
                    backend_str << host << ":" << port;
                    http_data_forwarder_->set_backend_for_request(client_fd, backend_str.str());
                });
        }
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
        add_backend(backend_cfg.host, backend_cfg.port, backend_cfg.weight);
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

    if (request_timeout_ms_ > 0) {
        lb::logging::Logger::instance().info(
            "Request timeout enabled: " + std::to_string(request_timeout_ms_) + "ms");
    }

    return true;
}

void LoadBalancer::apply_config(const std::shared_ptr<const lb::config::Config>& config) {
    if (!config)
        return;

    lb::logging::Logger::instance().info("Applying configuration reload");

    std::string new_mode = config->mode.empty() ? "tcp" : config->mode;
    if (new_mode != mode_) {
        lb::logging::Logger::instance().warn(
            "Mode change detected in config (current: \"" + mode_ + "\", new: \"" + new_mode +
            "\"). Mode changes require a restart to take effect. Current mode will remain active.");
    }

    max_global_connections_ = config->max_global_connections;
    max_connections_per_backend_ = config->max_connections_per_backend;
    backpressure_timeout_ms_ = config->backpressure_timeout_ms;

    backpressure_manager_ =
        std::make_unique<BackpressureManager>(backpressure_start_times_, backpressure_timeout_ms_);

    if (mode_ == "http" && http_data_forwarder_) {
        auto headers_to_add = config->http_request_headers_add;
        auto headers_to_remove = config->http_request_headers_remove;
        if (!headers_to_add.empty() || !headers_to_remove.empty()) {
            http_data_forwarder_->set_custom_header_modifier(
                [headers_to_add, headers_to_remove](lb::http::HttpRequest& request) {
                    lb::http::HttpHandler::apply_custom_headers(request, headers_to_add,
                                                                headers_to_remove);
                });
        } else {
            http_data_forwarder_->set_custom_header_modifier(nullptr);
        }
    }

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
            add_backend(backend_cfg.host, backend_cfg.port, backend_cfg.weight);
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

void LoadBalancer::check_request_timeouts() {
    if (request_timeout_ms_ == 0)
        return;

    auto now = std::chrono::steady_clock::now();
    std::vector<int> timed_out_fds;

    for (const auto& [fd, conn] : connections_) {
        if (backend_connections_.find(fd) != backend_connections_.end())
            continue;

        if (!conn || conn->state() != net::ConnectionState::ESTABLISHED)
            continue;

        auto it = connection_times_.find(fd);
        if (it != connection_times_.end()) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();

            if (elapsed >= static_cast<int64_t>(request_timeout_ms_)) {
                timed_out_fds.push_back(fd);
            }
        }
    }

    for (int fd : timed_out_fds) {
        lb::metrics::Metrics::instance().increment_request_timeouts();
        lb::logging::Logger::instance().warn("Request timeout for connection fd=" +
                                             std::to_string(fd));
        connection_manager_->close_connection(fd);
    }
}

void LoadBalancer::add_backend(const std::string& host, uint16_t port, uint32_t weight) {
    auto existing = backend_pool_->find_backend(host, port);
    if (existing)
        return;
    auto backend = std::make_shared<BackendNode>(host, port, weight);
    backend_pool_->add_backend(backend);

    if (health_checker_)
        health_checker_->add_backend(backend);

    lb::logging::Logger::instance().info("Added backend " + host + ":" + std::to_string(port) +
                                         " (weight=" + std::to_string(weight) + ")");
}

bool LoadBalancer::is_ip_allowed(const std::string& client_ip,
                                 const std::shared_ptr<const lb::config::Config>& config) const {
    if (!config || client_ip.empty())
        return true;

    for (const auto& blacklisted : config->ip_blacklist) {
        if (client_ip == blacklisted) {
            lb::logging::Logger::instance().warn("Connection rejected: IP " + client_ip +
                                                 " is blacklisted");
            return false;
        }
    }

    if (!config->ip_whitelist.empty()) {
        bool allowed = false;
        for (const auto& whitelisted : config->ip_whitelist) {
            if (client_ip == whitelisted) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            lb::logging::Logger::instance().warn("Connection rejected: IP " + client_ip +
                                                 " not in whitelist");
            return false;
        }
    }

    return true;
}

void LoadBalancer::send_http_error_and_close(int client_fd,
                                             const lb::http::HttpResponse& response) {
    auto response_bytes = response.to_bytes();
    ssize_t sent = 0;
    ssize_t total = response_bytes.size();

    while (sent < total) {
        ssize_t bytes = send(client_fd, response_bytes.data() + sent, total - sent, 0);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        sent += bytes;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ::close(client_fd);
}

bool LoadBalancer::check_rate_limit(const std::string& client_ip,
                                    const std::shared_ptr<const lb::config::Config>& config) {
    if (!config || !config->rate_limit_enabled || client_ip.empty() || client_ip == "unknown")
        return true;

    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(config->rate_limit_window_seconds);
    auto cutoff_time = now - window;

    std::lock_guard<std::mutex> lock(rate_limit_mutex_);

    auto& timestamps = rate_limit_tracker_[client_ip];

    timestamps.erase(std::remove_if(timestamps.begin(), timestamps.end(),
                                    [cutoff_time](const auto& ts) { return ts < cutoff_time; }),
                     timestamps.end());

    if (timestamps.size() >= config->rate_limit_max_connections) {
        lb::logging::Logger::instance().warn(
            "Connection rate limit exceeded for IP " + client_ip + " (" +
            std::to_string(timestamps.size()) + " connections in last " +
            std::to_string(config->rate_limit_window_seconds) + " seconds)");
        return false;
    }

    timestamps.push_back(now);
    return true;
}

void LoadBalancer::cleanup_rate_limit_entries() {
    auto config = config_manager_ ? config_manager_->get_config() : nullptr;
    if (!config || !config->rate_limit_enabled)
        return;

    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(config->rate_limit_window_seconds);
    auto cutoff_time = now - window;

    std::lock_guard<std::mutex> lock(rate_limit_mutex_);

    auto it = rate_limit_tracker_.begin();
    while (it != rate_limit_tracker_.end()) {
        auto& timestamps = it->second;

        timestamps.erase(std::remove_if(timestamps.begin(), timestamps.end(),
                                        [cutoff_time](const auto& ts) { return ts < cutoff_time; }),
                         timestamps.end());

        if (timestamps.empty()) {
            it = rate_limit_tracker_.erase(it);
        } else {
            ++it;
        }
    }
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

        std::string client_ip = lb::http::HttpHandler::extract_client_ip(client_fd);
        if (client_ip.empty())
            client_ip = "unknown";

        auto config = config_manager_ ? config_manager_->get_config() : nullptr;

        if (!is_ip_allowed(client_ip, config)) {
            lb::metrics::Metrics::instance().increment_overload_drops();

            if (config && config->mode == "http") {
                lb::http::HttpResponse error_resp =
                    lb::http::HttpResponse::forbidden("IP address not allowed");
                send_http_error_and_close(client_fd, error_resp);
            } else {
                ::close(client_fd);
            }
            continue;
        }

        if (!check_rate_limit(client_ip, config)) {
            lb::metrics::Metrics::instance().increment_rate_limit_drops();

            if (config && config->mode == "http") {
                lb::http::HttpResponse error_resp =
                    lb::http::HttpResponse::too_many_requests("Rate limit exceeded");
                send_http_error_and_close(client_fd, error_resp);
            } else {
                ::close(client_fd);
            }
            continue;
        }

        size_t established_count = connection_manager_->count_established_connections();
        if (established_count >= max_global_connections_) {
            lb::metrics::Metrics::instance().increment_overload_drops();
            lb::logging::Logger::instance().warn("Connection rejected: global connection limit (" +
                                                 std::to_string(max_global_connections_) +
                                                 ") exceeded");

            if (config && config->mode == "http") {
                lb::http::HttpResponse error_resp =
                    lb::http::HttpResponse::service_unavailable("Service temporarily unavailable");
                send_http_error_and_close(client_fd, error_resp);
            } else {
                ::close(client_fd);
            }
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
            connection_times_[client_fd_stored] = std::chrono::steady_clock::now();

            reactor_->add_fd(client_fd_stored, EPOLLIN | EPOLLOUT,
                             [this](int fd, net::EventType type) {
                                 event_handlers_->handle_client_event(fd, type);
                             });
        } else {
            client_conn->set_state(net::ConnectionState::ESTABLISHED);
            int client_fd_stored = client_conn->fd();
            connection_times_[client_fd_stored] = std::chrono::steady_clock::now();
            backend_connector_->connect(std::move(client_conn), 0);
        }
    }
}

} // namespace lb::core

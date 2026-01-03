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
#include "health/health_checker.h"
#include "metrics/metrics.h"

namespace lb::core {

LoadBalancer::LoadBalancer()
    : max_global_connections_(1000), max_connections_per_backend_(100),
      backpressure_timeout_ms_(10000), connection_timeout_seconds_(5), config_manager_(nullptr) {
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
        [this](int fd) { connection_manager_->close_connection(fd); },
        [this](int fd) { connection_manager_->close_backend_connection_only(fd); },
        [this](net::Connection* from, net::Connection* to) { data_forwarder_->forward(from, to); },
        [this](int fd) { backpressure_manager_->clear_tracking(fd); },
        [this](std::unique_ptr<net::Connection> conn, int retry_count) {
            retry_handler_->retry(std::move(conn), retry_count);
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
    for (auto& [fd, conn] : connections_)
        if (conn)
            conn->close();
    connections_.clear();
}

bool LoadBalancer::initialize(const std::string& listen_host, uint16_t listen_port) {
    listener_ = std::make_unique<net::TcpListener>();
    if (!listener_->bind(listen_host, listen_port))
        return false;
    if (!listener_->listen())
        return false;
    int listener_fd = listener_->fd();
    if (!reactor_->add_fd(listener_fd, EPOLLIN, [this](int fd, net::EventType type) {
            (void)fd;
            (void)type;
            handle_accept();
        })) {
        return false;
    }
    if (health_checker_)
        health_checker_->start();

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

    backpressure_manager_ =
        std::make_unique<BackpressureManager>(backpressure_start_times_, backpressure_timeout_ms_);

    for (const auto& backend_cfg : config->backends) {
        add_backend(backend_cfg.host, backend_cfg.port);
    }

    return true;
}

void LoadBalancer::apply_config(const std::shared_ptr<const lb::config::Config>& config) {
    if (!config)
        return;
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
            }
        }
    }

    for (const auto& backend_cfg : config->backends) {
        auto existing = backend_pool_->find_backend(backend_cfg.host, backend_cfg.port);
        if (!existing)
            add_backend(backend_cfg.host, backend_cfg.port);
        else if (existing->state() == BackendState::DRAINING)
            existing->set_state(BackendState::HEALTHY);
    }
    cleanup_drained_backends();
}

void LoadBalancer::cleanup_drained_backends() {
    auto all_backends = backend_pool_->get_all_backends();
    for (const auto& backend : all_backends) {
        if (backend->state() == BackendState::DRAINING && backend->active_connections() == 0) {
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
            ::close(client_fd);
            continue;
        }
        lb::metrics::Metrics::instance().increment_connections_total();
        auto client_conn = std::make_unique<net::Connection>(client_fd);
        client_conn->set_state(net::ConnectionState::ESTABLISHED);
        backend_connector_->connect(std::move(client_conn), 0);
    }
}

} // namespace lb::core

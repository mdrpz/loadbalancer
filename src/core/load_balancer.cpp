#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include "core/load_balancer.h"
#include "core/backend_node.h"
#include "health/health_checker.h"
#include "metrics/metrics.h"
#include "config/config.h"

namespace lb::core {

LoadBalancer::LoadBalancer() 
    : max_global_connections_(1000), max_connections_per_backend_(100),
      backpressure_timeout_ms_(10000), connection_timeout_seconds_(5),
      config_manager_(nullptr) {
    reactor_ = std::make_unique<net::EpollReactor>();
    backend_pool_ = std::make_unique<BackendPool>();
    health_checker_ = std::make_unique<lb::health::HealthChecker>();
    
    // Initialize helper classes in dependency order
    connection_manager_ = std::make_unique<ConnectionManager>(
        connections_, backend_connections_, connection_times_,
        backpressure_start_times_, client_retry_counts_,
        backend_to_client_map_, *reactor_);
    
    backpressure_manager_ = std::make_unique<BackpressureManager>(
        backpressure_start_times_, backpressure_timeout_ms_);
    
    data_forwarder_ = std::make_unique<DataForwarder>(
        *reactor_,
        [this](int fd) { backpressure_manager_->start_tracking(fd); },
        [this](int fd) { backpressure_manager_->clear_tracking(fd); },
        [this](int fd) { connection_manager_->close_connection(fd); });
    
    // Create retry handler with lambda that will be set up after backend_connector
    retry_handler_ = std::make_unique<RetryHandler>(
        [this](std::unique_ptr<net::Connection> conn, int retry_count) {
            backend_connector_->connect(std::move(conn), retry_count);
        });
    
    // Create event handlers with lambda that will be set up after retry_handler
    event_handlers_ = std::make_unique<EventHandlers>(
        connections_, backend_connections_, connection_times_,
        backend_to_client_map_, client_retry_counts_,
        *reactor_, connection_timeout_seconds_,
        [this](int fd) { return connection_manager_->get_connection(fd); },
        [this](int fd) { backpressure_manager_->check_timeout(fd, [this](int f) { connection_manager_->close_connection(f); }); },
        [this](int fd) { connection_manager_->close_connection(fd); },
        [this](int fd) { connection_manager_->close_backend_connection_only(fd); },
        [this](net::Connection* from, net::Connection* to) { data_forwarder_->forward(from, to); },
        [this](int fd) { backpressure_manager_->clear_tracking(fd); },
        [this](std::unique_ptr<net::Connection> conn, int retry_count) { retry_handler_->retry(std::move(conn), retry_count); });
    
    // Create backend connector last (depends on event_handlers and retry_handler)
    backend_connector_ = std::make_unique<BackendConnector>(
        connections_, backend_connections_, connection_times_,
        backend_to_client_map_, client_retry_counts_,
        *backend_pool_, *reactor_, max_connections_per_backend_,
        [this](int fd, net::EventType type) { event_handlers_->handle_client_event(fd, type); },
        [this](int fd, net::EventType type) { event_handlers_->handle_backend_event(fd, type); },
        [this](std::unique_ptr<net::Connection> conn, int retry_count) { retry_handler_->retry(std::move(conn), retry_count); });
}

LoadBalancer::~LoadBalancer() {
    // Stop health checker
    if (health_checker_) {
        health_checker_->stop();
    }
    
    // Close all connections
    for (auto& [fd, conn] : connections_) {
        if (conn) {
            conn->close();
        }
    }
    connections_.clear();
}

bool LoadBalancer::initialize(const std::string& listen_host, uint16_t listen_port) {
    listener_ = std::make_unique<net::TcpListener>();
    
    if (!listener_->bind(listen_host, listen_port)) {
        return false;
    }
    
    if (!listener_->listen()) {
        return false;
    }
    
    // Register listener fd with reactor for accept events
    int listener_fd = listener_->fd();
    if (!reactor_->add_fd(listener_fd, EPOLLIN, 
                          [this](int fd, net::EventType type) {
                              (void)fd;
                              (void)type;
                              handle_accept();
                          })) {
        return false;
    }
    
    // Start health checker
    if (health_checker_) {
        health_checker_->start();
    }
    
    return true;
}

void LoadBalancer::set_config_manager(lb::config::ConfigManager* config_manager) {
    config_manager_ = config_manager;
    
    // Set up periodic config reload check (every 5 seconds)
    if (reactor_ && config_manager_) {
        reactor_->set_periodic_callback([this]() {
            if (config_manager_ && config_manager_->check_and_reload()) {
                // Config was reloaded - apply changes
                auto new_config = config_manager_->get_config();
                if (new_config) {
                    apply_config(new_config);
                }
            }
            // Also cleanup DRAINING backends periodically
            cleanup_drained_backends();
        }, 5000); // Check every 5 seconds
    }
}

void LoadBalancer::run() {
    if (reactor_) {
        reactor_->run();
    }
}

void LoadBalancer::stop() {
    // Stop health checker
    if (health_checker_) {
        health_checker_->stop();
    }
    
    // Stop reactor
    if (reactor_) {
        reactor_->stop();
    }
}

bool LoadBalancer::initialize_from_config(std::shared_ptr<const lb::config::Config> config) {
    if (!config) {
        return false;
    }
    
    // Initialize listener
    if (!initialize(config->listen_host, config->listen_port)) {
        return false;
    }
    
    // Apply config values
    max_global_connections_ = config->max_global_connections;
    max_connections_per_backend_ = config->max_connections_per_backend;
    backpressure_timeout_ms_ = config->backpressure_timeout_ms;
    
    // Update backpressure manager timeout
    backpressure_manager_ = std::make_unique<BackpressureManager>(
        backpressure_start_times_, backpressure_timeout_ms_);
    
    // Add backends
    for (const auto& backend_cfg : config->backends) {
        add_backend(backend_cfg.host, backend_cfg.port);
    }
    
    return true;
}

void LoadBalancer::apply_config(std::shared_ptr<const lb::config::Config> config) {
    if (!config) {
        return;
    }
    
    // Update connection limits
    max_global_connections_ = config->max_global_connections;
    max_connections_per_backend_ = config->max_connections_per_backend;
    backpressure_timeout_ms_ = config->backpressure_timeout_ms;
    
    // Update backpressure manager
    backpressure_manager_ = std::make_unique<BackpressureManager>(
        backpressure_start_times_, backpressure_timeout_ms_);
    
    // Handle backend changes: add new, mark removed as DRAINING
    // Get current backends
    auto current_backends = backend_pool_->get_all_backends();
    
    // Create set of new backend keys (host:port)
    std::set<std::pair<std::string, uint16_t>> new_backend_keys;
    for (const auto& backend_cfg : config->backends) {
        new_backend_keys.insert({backend_cfg.host, backend_cfg.port});
    }
    
    // Mark removed backends as DRAINING
    for (const auto& backend : current_backends) {
        std::pair<std::string, uint16_t> key = {backend->host(), backend->port()};
        if (new_backend_keys.find(key) == new_backend_keys.end()) {
            // Backend not in new config - mark as DRAINING
            if (backend->state() != BackendState::DRAINING) {
                backend->set_state(BackendState::DRAINING);
            }
        }
    }
    
    // Add new backends that don't exist
    for (const auto& backend_cfg : config->backends) {
        auto existing = backend_pool_->find_backend(backend_cfg.host, backend_cfg.port);
        if (!existing) {
            // New backend - add it
            add_backend(backend_cfg.host, backend_cfg.port);
        } else if (existing->state() == BackendState::DRAINING) {
            // Backend was DRAINING but is back in config - restore to HEALTHY
            existing->set_state(BackendState::HEALTHY);
        }
    }
    
    // Clean up DRAINING backends with no active connections
    cleanup_drained_backends();
}

void LoadBalancer::cleanup_drained_backends() {
    // Remove DRAINING backends that have no active connections
    auto all_backends = backend_pool_->get_all_backends();
    for (const auto& backend : all_backends) {
        if (backend->state() == BackendState::DRAINING && 
            backend->active_connections() == 0) {
            // Backend is DRAINING and has no active connections - remove it
            backend_pool_->remove_backend(backend->host(), backend->port());
            
            // Remove from health checker
            if (health_checker_) {
                health_checker_->remove_backend(backend);
            }
        }
    }
}

void LoadBalancer::add_backend(const std::string& host, uint16_t port) {
    // Check if backend already exists
    auto existing = backend_pool_->find_backend(host, port);
    if (existing) {
        return; // Already exists
    }
    auto backend = std::make_shared<BackendNode>(host, port);
    backend_pool_->add_backend(backend);
    
    // Register with health checker
    if (health_checker_) {
        health_checker_->add_backend(backend);
    }
}

void LoadBalancer::handle_accept() {
    while (true) {
        int client_fd = listener_->accept();
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more connections to accept
            }
            break; // Error accepting
        }

        // Check global connection limit
        // Count established connection pairs (client + backend = 1 connection)
        size_t established_count = connection_manager_->count_established_connections();
        if (established_count >= max_global_connections_) {
            // Reject connection - close immediately
            lb::metrics::Metrics::instance().increment_overload_drops();
            ::close(client_fd);
            continue;
        }

        // Increment total connections metric
        lb::metrics::Metrics::instance().increment_connections_total();

        // Create client connection
        auto client_conn = std::make_unique<net::Connection>(client_fd);
        client_conn->set_state(net::ConnectionState::ESTABLISHED);
        
        // Route to backend
        backend_connector_->connect(std::move(client_conn), 0);
    }
}


} // namespace lb::core


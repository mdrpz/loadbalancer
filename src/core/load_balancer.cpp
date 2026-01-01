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

namespace lb::core {

LoadBalancer::LoadBalancer() 
    : max_global_connections_(1000), max_connections_per_backend_(100),
      backpressure_timeout_ms_(10000), connection_timeout_seconds_(5) {
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

void LoadBalancer::add_backend(const std::string& host, uint16_t port) {
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


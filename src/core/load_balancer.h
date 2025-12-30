#pragma once

#include "net/epoll_reactor.h"
#include "net/tcp_listener.h"
#include "net/connection.h"
#include "core/backend_pool.h"
#include "core/backend_node.h"
#include "health/health_checker.h"
#include <memory>
#include <unordered_map>
#include <chrono>

namespace lb::core {

class LoadBalancer {
public:
    LoadBalancer();
    ~LoadBalancer();

    bool initialize(const std::string& listen_host, uint16_t listen_port);
    void run();
    void stop();

    // Add backend for routing
    void add_backend(const std::string& host, uint16_t port);

private:
    // Connection management
    void handle_accept();
    void handle_client_event(int fd, net::EventType type);
    void handle_backend_event(int fd, net::EventType type);
    void connect_to_backend_with_retry(std::unique_ptr<net::Connection> client_conn, int retry_count = 0);
    void forward_data(net::Connection* from, net::Connection* to);
    void close_connection(int fd);
    void close_backend_connection_only(int backend_fd);
    
    // Helper to get connection by fd
    net::Connection* get_connection(int fd);

    std::unique_ptr<net::EpollReactor> reactor_;
    std::unique_ptr<net::TcpListener> listener_;
    std::unique_ptr<BackendPool> backend_pool_;
    std::unique_ptr<lb::health::HealthChecker> health_checker_;
    
    // Track all active connections
    std::unordered_map<int, std::unique_ptr<net::Connection>> connections_;
    
    // Track backend node for each backend connection (fd -> backend_node)
    std::unordered_map<int, std::weak_ptr<BackendNode>> backend_connections_;
    
    // Track connection start time for timeout (fd -> timestamp)
    std::unordered_map<int, std::chrono::steady_clock::time_point> connection_times_;
    
    // Track client connection for each backend connection (backend_fd -> client_fd)
    // Used for retry logic
    std::unordered_map<int, int> backend_to_client_map_;
    
    // Track retry count for client connections (client_fd -> retry_count)
    std::unordered_map<int, int> client_retry_counts_;
    
    // Connection limits
    uint32_t max_global_connections_;
    uint32_t max_connections_per_backend_;
    
    // Backpressure tracking
    std::unordered_map<int, std::chrono::steady_clock::time_point> backpressure_start_times_;
    uint32_t backpressure_timeout_ms_;
    
    // Helper to count established connections (client + backend pairs)
    size_t count_established_connections() const;
    
    // Backpressure helpers
    void check_backpressure_timeout(int fd);
    void start_backpressure_tracking(int fd);
    void clear_backpressure_tracking(int fd);
    
    // Failure handling and retry
    void retry_with_next_backend(std::unique_ptr<net::Connection> client_conn, int retry_count);
    
    // Retry configuration
    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    uint32_t connection_timeout_seconds_;
};

} // namespace lb::core


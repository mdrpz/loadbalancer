#pragma once

#include "net/epoll_reactor.h"
#include "net/tcp_listener.h"
#include "net/connection.h"
#include "core/backend_pool.h"
#include "core/backend_node.h"
#include "health/health_checker.h"
#include "core/connection_manager.h"
#include "core/event_handlers.h"
#include "core/backend_connector.h"
#include "core/data_forwarder.h"
#include "core/backpressure_manager.h"
#include "core/retry_handler.h"
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
    
    // Retry configuration
    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    uint32_t connection_timeout_seconds_;
    
    // Helper classes
    std::unique_ptr<ConnectionManager> connection_manager_;
    std::unique_ptr<EventHandlers> event_handlers_;
    std::unique_ptr<BackendConnector> backend_connector_;
    std::unique_ptr<DataForwarder> data_forwarder_;
    std::unique_ptr<BackpressureManager> backpressure_manager_;
    std::unique_ptr<RetryHandler> retry_handler_;
};

} // namespace lb::core


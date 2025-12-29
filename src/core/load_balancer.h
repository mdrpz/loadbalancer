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
    void connect_to_backend(std::unique_ptr<net::Connection> client_conn);
    void forward_data(net::Connection* from, net::Connection* to);
    void close_connection(int fd);
    
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
    
    // Connection limits
    uint32_t max_global_connections_;
    uint32_t max_connections_per_backend_;
    
    // Helper to count established connections (client + backend pairs)
    size_t count_established_connections() const;
};

} // namespace lb::core


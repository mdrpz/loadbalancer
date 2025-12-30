#pragma once

#include "net/connection.h"
#include "net/epoll_reactor.h"
#include "core/backend_node.h"
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>

namespace lb::core {

class ConnectionManager {
public:
    ConnectionManager(
        std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
        std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
        std::unordered_map<int, int>& client_retry_counts,
        std::unordered_map<int, int>& backend_to_client_map,
        net::EpollReactor& reactor);
    
    net::Connection* get_connection(int fd);
    size_t count_established_connections() const;
    
    void close_connection(int fd);
    void close_backend_connection_only(int backend_fd);

private:
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections_;
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times_;
    std::unordered_map<int, int>& client_retry_counts_;
    std::unordered_map<int, int>& backend_to_client_map_;
    net::EpollReactor& reactor_;
};

} // namespace lb::core


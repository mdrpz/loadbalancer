#pragma once

#include "net/connection.h"
#include "net/epoll_reactor.h"
#include "core/backend_node.h"
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>
#include <cstdint>

namespace lb::core {

class EventHandlers {
public:
    EventHandlers(
        std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
        std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
        std::unordered_map<int, int>& backend_to_client_map,
        std::unordered_map<int, int>& client_retry_counts,
        net::EpollReactor& reactor,
        uint32_t connection_timeout_seconds,
        std::function<net::Connection*(int)> get_connection,
        std::function<void(int)> check_backpressure,
        std::function<void(int)> close_connection,
        std::function<void(int)> close_backend_only,
        std::function<void(net::Connection*, net::Connection*)> forward_data,
        std::function<void(int)> clear_backpressure,
        std::function<void(std::unique_ptr<net::Connection>, int)> retry);
    
    void handle_client_event(int fd, net::EventType type);
    void handle_backend_event(int fd, net::EventType type);

private:
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections_;
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times_;
    std::unordered_map<int, int>& backend_to_client_map_;
    std::unordered_map<int, int>& client_retry_counts_;
    net::EpollReactor& reactor_;
    uint32_t connection_timeout_seconds_;
    
    std::function<net::Connection*(int)> get_connection_;
    std::function<void(int)> check_backpressure_;
    std::function<void(int)> close_connection_;
    std::function<void(int)> close_backend_only_;
    std::function<void(net::Connection*, net::Connection*)> forward_data_;
    std::function<void(int)> clear_backpressure_;
    std::function<void(std::unique_ptr<net::Connection>, int)> retry_;
    
    bool try_retry_on_backend_error(int backend_fd);
};

} // namespace lb::core


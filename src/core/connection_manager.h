#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include "core/backend_node.h"
#include "core/http_data_forwarder.h"
#include "net/connection.h"
#include "net/epoll_reactor.h"

namespace lb::core {

class ConnectionManager {
public:
    ConnectionManager(
        std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
        std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
        std::unordered_map<int, int>& client_retry_counts,
        std::unordered_map<int, int>& backend_to_client_map, net::EpollReactor& reactor);

    net::Connection* get_connection(int fd);
    [[nodiscard]] size_t count_established_connections() const;

    void close_connection(int fd);
    void close_backend_connection_only(int backend_fd);

    using PoolReleaseCallback =
        std::function<bool(int backend_fd, std::unique_ptr<net::Connection> conn)>;
    void set_pool_release_callback(PoolReleaseCallback callback);

    using AccessLogCallback = std::function<RequestInfo*(int client_fd)>;
    void set_access_log_callback(AccessLogCallback callback);

    using ClearRequestInfoCallback = std::function<void(int client_fd)>;
    void set_clear_request_info_callback(ClearRequestInfoCallback callback);

private:
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections_;
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times_;
    std::unordered_map<int, int>& client_retry_counts_;
    std::unordered_map<int, int>& backend_to_client_map_;
    net::EpollReactor& reactor_;
    PoolReleaseCallback pool_release_callback_;
    AccessLogCallback access_log_callback_;
    ClearRequestInfoCallback clear_request_info_callback_;
};

} // namespace lb::core

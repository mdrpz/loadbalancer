#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include "core/backend_pool.h"
#include "core/connection_pool.h"
#include "core/session_manager.h"
#include "net/connection.h"
#include "net/epoll_reactor.h"

namespace lb::core {

class BackendConnector {
public:
    BackendConnector(
        std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
        std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
        std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
        std::unordered_map<int, int>& backend_to_client_map,
        std::unordered_map<int, int>& client_retry_counts, BackendPool& backend_pool,
        net::EpollReactor& reactor, uint32_t max_connections_per_backend,
        std::function<void(int, net::EventType)> client_handler,
        std::function<void(int, net::EventType)> backend_handler,
        std::function<void(std::unique_ptr<net::Connection>, int)> retry_callback);

    void connect(std::unique_ptr<net::Connection> client_conn, int retry_count,
                 const std::string& session_key = "");

    void set_pool_manager(ConnectionPoolManager* pool_manager);
    void set_session_manager(SessionManager* session_manager);
    void set_sticky_config(bool enabled, const std::string& method, uint32_t ttl_seconds);
    void set_connection_init_callback(std::function<void(net::Connection*)> callback);

    using BackendSelectedCallback =
        std::function<void(int client_fd, const std::string& host, uint16_t port)>;
    void set_backend_selected_callback(BackendSelectedCallback callback);

    void set_max_connections_per_backend(uint32_t max_connections);
    void set_backend_socket_sndbuf(uint32_t sndbuf);

    struct BackendInfo {
        std::string host;
        uint16_t port;
        bool pooled;
    };
    BackendInfo get_backend_info(int backend_fd) const;

private:
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections_;
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections_;
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times_;
    std::unordered_map<int, int>& backend_to_client_map_;
    std::unordered_map<int, int>& client_retry_counts_;
    BackendPool& backend_pool_;
    net::EpollReactor& reactor_;
    uint32_t max_connections_per_backend_;
    std::function<void(int, net::EventType)> client_handler_;
    std::function<void(int, net::EventType)> backend_handler_;
    std::function<void(std::unique_ptr<net::Connection>, int)> retry_callback_;

    ConnectionPoolManager* pool_manager_ = nullptr;
    std::unordered_map<int, BackendInfo> pooled_connections_;
    BackendSelectedCallback backend_selected_callback_;
    std::function<void(net::Connection*)> connection_init_callback_;
    SessionManager* session_manager_ = nullptr;
    uint32_t backend_socket_sndbuf_ = 0;
    bool sticky_sessions_enabled_ = false;
    std::string sticky_sessions_method_;
    uint32_t sticky_sessions_ttl_seconds_ = 3600;
};

} // namespace lb::core

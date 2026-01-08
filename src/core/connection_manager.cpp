#include "core/connection_manager.h"
#include "logging/logger.h"
#include "metrics/metrics.h"

namespace lb::core {

ConnectionManager::ConnectionManager(
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
    std::unordered_map<int, int>& client_retry_counts,
    std::unordered_map<int, int>& backend_to_client_map, net::EpollReactor& reactor)
    : connections_(connections), backend_connections_(backend_connections),
      connection_times_(connection_times), backpressure_times_(backpressure_times),
      client_retry_counts_(client_retry_counts), backend_to_client_map_(backend_to_client_map),
      reactor_(reactor) {}

void ConnectionManager::set_pool_release_callback(PoolReleaseCallback callback) {
    pool_release_callback_ = std::move(callback);
}

net::Connection* ConnectionManager::get_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return nullptr;
    return it->second.get();
}

size_t ConnectionManager::count_established_connections() const {
    size_t count = 0;
    for (const auto& [fd, conn] : connections_) {
        if (backend_connections_.find(fd) == backend_connections_.end() && conn &&
            conn->state() == net::ConnectionState::ESTABLISHED)
            count++;
    }
    return count;
}

void ConnectionManager::close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;

    auto* conn = it->second.get();
    if (!conn) {
        connections_.erase(it);
        return;
    }

    if (conn->state() == net::ConnectionState::CLOSED) {
        backend_connections_.erase(fd);
        connection_times_.erase(fd);
        backpressure_times_.erase(fd);
        client_retry_counts_.erase(fd);
        backend_to_client_map_.erase(fd);
        connections_.erase(it);
        return;
    }

    conn->set_state(net::ConnectionState::CLOSED);

    auto backend_it = backend_connections_.find(fd);
    bool is_client_connection = (backend_it == backend_connections_.end());

    std::string backend_info;
    if (backend_it != backend_connections_.end()) {
        if (auto backend_node = backend_it->second.lock()) {
            backend_node->decrement_connections();
            backend_info =
                " backend=" + backend_node->host() + ":" + std::to_string(backend_node->port());
        }
        backend_connections_.erase(backend_it);
    }

    if (is_client_connection) {
        lb::metrics::Metrics::instance().decrement_connections_active();
        lb::logging::Logger::instance().debug("Client connection closed (fd=" + std::to_string(fd) +
                                              ")");
    } else {
        lb::logging::Logger::instance().debug(
            "Backend connection closed (fd=" + std::to_string(fd) + backend_info + ")");
    }

    connection_times_.erase(fd);

    backpressure_times_.erase(fd);

    client_retry_counts_.erase(fd);
    backend_to_client_map_.erase(fd);

    for (auto it = backend_to_client_map_.begin(); it != backend_to_client_map_.end();) {
        if (it->second == fd)
            it = backend_to_client_map_.erase(it);
        else
            ++it;
    }

    if (conn->peer()) {
        int peer_fd = conn->peer()->fd();

        conn->set_peer(nullptr);

        auto peer_it = connections_.find(peer_fd);
        if (peer_it != connections_.end()) {
            if (peer_it->second && peer_it->second->state() != net::ConnectionState::CLOSED) {
                peer_it->second->set_peer(nullptr);
                auto peer_backend_it = backend_connections_.find(peer_fd);
                bool is_peer_backend = (peer_backend_it != backend_connections_.end());

                if (peer_backend_it != backend_connections_.end()) {
                    if (auto backend_node = peer_backend_it->second.lock()) {
                        backend_node->decrement_connections();
                    }
                    backend_connections_.erase(peer_backend_it);
                }
                connection_times_.erase(peer_fd);
                backpressure_times_.erase(peer_fd);
                client_retry_counts_.erase(peer_fd);
                backend_to_client_map_.erase(peer_fd);
                reactor_.del_fd(peer_fd);

                bool released_to_pool = false;
                if (is_client_connection && is_peer_backend && pool_release_callback_) {
                    auto peer_conn = std::move(peer_it->second);
                    released_to_pool = pool_release_callback_(peer_fd, std::move(peer_conn));
                    if (released_to_pool) {
                        lb::logging::Logger::instance().debug(
                            "Backend connection released to pool (fd=" + std::to_string(peer_fd) +
                            ")");
                    }
                    connections_.erase(peer_it);
                } else {
                    peer_it->second->close();
                    connections_.erase(peer_it);
                }
            } else {
                connections_.erase(peer_it);
            }
        }
    }

    reactor_.del_fd(fd);
    conn->close();
    connections_.erase(it);
}

void ConnectionManager::close_backend_connection_only(int backend_fd) {
    auto it = connections_.find(backend_fd);
    if (it == connections_.end())
        return;

    auto* conn = it->second.get();
    if (conn) {
        if (conn->peer())
            conn->peer()->set_peer(nullptr);
        conn->set_state(net::ConnectionState::CLOSED);
        auto backend_it = backend_connections_.find(backend_fd);
        if (backend_it != backend_connections_.end()) {
            if (auto backend_node = backend_it->second.lock()) {
                backend_node->decrement_connections();
            }
            backend_connections_.erase(backend_it);
        }
        connection_times_.erase(backend_fd);
        backpressure_times_.erase(backend_fd);
        backend_to_client_map_.erase(backend_fd);
        reactor_.del_fd(backend_fd);
        conn->close();
    }
    connections_.erase(it);
}

} // namespace lb::core

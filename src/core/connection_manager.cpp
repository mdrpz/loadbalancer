#include "core/connection_manager.h"
#include "logging/access_logger.h"
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

void ConnectionManager::set_access_log_callback(AccessLogCallback callback) {
    access_log_callback_ = std::move(callback);
}

void ConnectionManager::set_clear_request_info_callback(ClearRequestInfoCallback callback) {
    clear_request_info_callback_ = std::move(callback);
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

    auto backend_it = backend_connections_.find(fd);
    bool was_established = (conn->state() == net::ConnectionState::ESTABLISHED);
    bool is_client_connection = (backend_it == backend_connections_.end());

    std::string backend_info;
    if (backend_it != backend_connections_.end()) {
        if (auto backend_node = backend_it->second.lock()) {
            if (was_established) {
                backend_node->decrement_connections();
            }
            backend_info =
                " backend=" + backend_node->host() + ":" + std::to_string(backend_node->port());
        }
        backend_connections_.erase(backend_it);
    }

    conn->set_state(net::ConnectionState::CLOSED);

    lb::metrics::Metrics::instance().add_bytes_in(conn->bytes_read());
    lb::metrics::Metrics::instance().add_bytes_out(conn->bytes_written());

    if (is_client_connection) {
        if (access_log_callback_) {
            RequestInfo* req_info = access_log_callback_(fd);
            if (req_info && !req_info->method.empty() && !req_info->response_complete) {
                auto end_time =
                    req_info->response_complete &&
                            req_info->response_complete_time.time_since_epoch().count() > 0
                        ? req_info->response_complete_time
                        : std::chrono::system_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    end_time - req_info->start_time);

                lb::logging::AccessLogEntry entry;
                entry.client_ip = req_info->client_ip;
                entry.method = req_info->method;
                entry.path = req_info->path;
                entry.status_code = req_info->status_code > 0 ? req_info->status_code : 0;
                entry.bytes_sent = conn->bytes_written();
                entry.bytes_received = conn->bytes_read();
                entry.latency = latency;
                entry.backend = req_info->backend;
                entry.timestamp = req_info->start_time;

                lb::logging::AccessLogger::instance().log(entry);

                if (latency.count() > 0) {
                    double latency_ms = latency.count() / 1000.0;
                    lb::metrics::Metrics::instance().record_request_latency_ms(latency_ms);
                }

                if (clear_request_info_callback_) {
                    clear_request_info_callback_(fd);
                }
            }
        }

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
                bool peer_was_established =
                    (peer_it->second->state() == net::ConnectionState::ESTABLISHED);
                peer_it->second->set_peer(nullptr);
                auto peer_backend_it = backend_connections_.find(peer_fd);
                bool is_peer_backend = (peer_backend_it != backend_connections_.end());

                if (peer_backend_it != backend_connections_.end()) {
                    if (auto backend_node = peer_backend_it->second.lock()) {
                        if (peer_was_established) {
                            backend_node->decrement_connections();
                        }
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
        bool was_established = (conn->state() == net::ConnectionState::ESTABLISHED);
        if (conn->peer())
            conn->peer()->set_peer(nullptr);
        conn->set_state(net::ConnectionState::CLOSED);
        auto backend_it = backend_connections_.find(backend_fd);
        if (backend_it != backend_connections_.end()) {
            if (auto backend_node = backend_it->second.lock()) {
                if (was_established) {
                    backend_node->decrement_connections();
                }
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

#include "core/connection_manager.h"

namespace lb::core {

ConnectionManager::ConnectionManager(
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
    std::unordered_map<int, int>& client_retry_counts,
    std::unordered_map<int, int>& backend_to_client_map,
    net::EpollReactor& reactor)
    : connections_(connections),
      backend_connections_(backend_connections),
      connection_times_(connection_times),
      backpressure_times_(backpressure_times),
      client_retry_counts_(client_retry_counts),
      backend_to_client_map_(backend_to_client_map),
      reactor_(reactor) {
}

net::Connection* ConnectionManager::get_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return nullptr;
    }
    return it->second.get();
}

size_t ConnectionManager::count_established_connections() const {
    // Count client connections that are established
    // Client connections are those not in backend_connections_ map
    // We count all established client connections (even if backend is still connecting)
    size_t count = 0;
    for (const auto& [fd, conn] : connections_) {
        // Check if this is a client connection (not a backend connection)
        if (backend_connections_.find(fd) == backend_connections_.end()) {
            // This is a client connection
            if (conn && conn->state() == net::ConnectionState::ESTABLISHED) {
                count++;
            }
        }
    }
    return count;
}

void ConnectionManager::close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return; // Already closed or never existed
    }

    auto* conn = it->second.get();
    if (!conn) {
        connections_.erase(it);
        return;
    }

    // Mark connection as closed to prevent race conditions
    if (conn->state() == net::ConnectionState::CLOSED) {
        // Already being closed or closed - make cleanup idempotent
        // Just clean up tracking maps
        backend_connections_.erase(fd);
        connection_times_.erase(fd);
        backpressure_times_.erase(fd);
        client_retry_counts_.erase(fd);
        backend_to_client_map_.erase(fd);
        connections_.erase(it);
        return;
    }

    // Mark as closed before cleanup to prevent races
    conn->set_state(net::ConnectionState::CLOSED);
    
    // If this is a backend connection, decrement connection count
    auto backend_it = backend_connections_.find(fd);
    if (backend_it != backend_connections_.end()) {
        if (auto backend_node = backend_it->second.lock()) {
            backend_node->decrement_connections();
        }
        backend_connections_.erase(backend_it);
    }
    
    // Clean up connection time tracking
    connection_times_.erase(fd);
    
    // Clean up backpressure tracking
    backpressure_times_.erase(fd);
    
    // Clean up retry tracking
    client_retry_counts_.erase(fd);
    backend_to_client_map_.erase(fd);
    
    // Also remove from backend_to_client_map if this is a backend connection
    for (auto it = backend_to_client_map_.begin(); it != backend_to_client_map_.end();) {
        if (it->second == fd) {
            it = backend_to_client_map_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Handle peer connection cleanup (with race condition protection)
    if (conn->peer()) {
        int peer_fd = conn->peer()->fd();
        
        // Clear peer pointer to break circular reference
        conn->set_peer(nullptr);
        
        // Check if peer connection still exists and isn't already closed
        auto peer_it = connections_.find(peer_fd);
        if (peer_it != connections_.end()) {
            auto* peer_conn = peer_it->second.get();
            if (peer_conn && peer_conn->state() != net::ConnectionState::CLOSED) {
                // Clear peer's pointer to us
                peer_conn->set_peer(nullptr);
                
                // Clean up peer's backend tracking if it's a backend connection
                auto peer_backend_it = backend_connections_.find(peer_fd);
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
                
                // Remove from reactor (safe even if already removed)
                reactor_.del_fd(peer_fd);
                
                // Close and remove peer connection
                peer_conn->close();
                connections_.erase(peer_it);
            }
        }
    }

    // Remove from reactor (safe even if already removed)
    reactor_.del_fd(fd);
    
    // Close connection and remove from map
    conn->close();
    connections_.erase(it);
}

void ConnectionManager::close_backend_connection_only(int backend_fd) {
    // Close only the backend connection, not the client
    // Used when retrying with a different backend
    auto it = connections_.find(backend_fd);
    if (it == connections_.end()) {
        return;
    }
    
    auto* conn = it->second.get();
    if (conn) {
        // Clear peer pointer
        if (conn->peer()) {
            conn->peer()->set_peer(nullptr);
        }
        
        // Mark as closed
        conn->set_state(net::ConnectionState::CLOSED);
        
        // Clean up backend tracking
        auto backend_it = backend_connections_.find(backend_fd);
        if (backend_it != backend_connections_.end()) {
            if (auto backend_node = backend_it->second.lock()) {
                backend_node->decrement_connections();
            }
            backend_connections_.erase(backend_it);
        }
        
        // Clean up tracking maps
        connection_times_.erase(backend_fd);
        backpressure_times_.erase(backend_fd);
        backend_to_client_map_.erase(backend_fd);
        
        // Remove from reactor
        reactor_.del_fd(backend_fd);
        
        // Close socket
        conn->close();
    }
    
    // Remove from connections map
    connections_.erase(it);
}

} // namespace lb::core


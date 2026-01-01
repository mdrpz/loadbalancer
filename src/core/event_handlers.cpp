#include "core/event_handlers.h"
#include "metrics/metrics.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <chrono>
#include <sstream>

namespace lb::core {

EventHandlers::EventHandlers(
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
    std::function<void(std::unique_ptr<net::Connection>, int)> retry)
    : connections_(connections),
      backend_connections_(backend_connections),
      connection_times_(connection_times),
      backend_to_client_map_(backend_to_client_map),
      client_retry_counts_(client_retry_counts),
      reactor_(reactor),
      connection_timeout_seconds_(connection_timeout_seconds),
      get_connection_(get_connection),
      check_backpressure_(check_backpressure),
      close_connection_(close_connection),
      close_backend_only_(close_backend_only),
      forward_data_(forward_data),
      clear_backpressure_(clear_backpressure),
      retry_(retry) {
}

bool EventHandlers::try_retry_on_backend_error(int backend_fd) {
    // Get client connection for retry
    auto client_it = backend_to_client_map_.find(backend_fd);
    if (client_it == backend_to_client_map_.end()) {
        return false;
    }
    
    int client_fd = client_it->second;
    auto client_conn_it = connections_.find(client_fd);
    if (client_conn_it == connections_.end()) {
        return false;
    }
    
    // Get retry count
    int retry_count = 0;
    auto retry_it = client_retry_counts_.find(client_fd);
    if (retry_it != client_retry_counts_.end()) {
        retry_count = retry_it->second;
    }
    
    // Remove backend connection before retry
    close_backend_only_(backend_fd);
    
    // Retry with next backend
    auto client_conn = std::move(client_conn_it->second);
    connections_.erase(client_conn_it);
    retry_(std::move(client_conn), retry_count);
    return true;
}

void EventHandlers::handle_client_event(int fd, net::EventType type) {
    auto* conn = get_connection_(fd);
    if (!conn) {
        return;
    }

    // Validate connection state
    if (conn->state() == net::ConnectionState::CLOSED) {
        return;
    }

    // Check backpressure timeout before processing events
    check_backpressure_(fd);

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection_(fd);
        return;
    }

    if (type == net::EventType::READ) {
        // Only read if connection is established
        if (conn->state() != net::ConnectionState::ESTABLISHED) {
            return;
        }

        // Read from client
        bool read_success = conn->read_from_fd();
        
        // Forward any data that was read (even if EOF was received)
        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
            if (!conn->read_buffer().empty()) {
                forward_data_(conn, conn->peer());
            }
        }
        
        // Handle EOF from client - shutdown write side but keep connection open to receive backend response
        if (!read_success) {
            // Forward any remaining data in read buffer before shutting down
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                if (!conn->read_buffer().empty()) {
                    forward_data_(conn, conn->peer());
                }
            }
            
            // Shutdown write side of client socket (no more data to send)
            if (conn->fd() >= 0) {
                ::shutdown(conn->fd(), SHUT_WR);
            }
            // Shutdown write side of backend socket (no more data will come from client)
            if (conn->peer() && conn->peer()->fd() >= 0) {
                ::shutdown(conn->peer()->fd(), SHUT_WR);
            }
            // Don't close yet - wait for backend to finish sending response
            // The connection will be closed when backend sends EOF
            return;
        }
    }

    if (type == net::EventType::WRITE) {
        // Only write if connection is established
        if (conn->state() != net::ConnectionState::ESTABLISHED) {
            return;
        }

        // Write to client
        if (!conn->write_to_fd()) {
            // Error writing
            close_connection_(fd);
            return;
        }

        // If write buffer is empty, clear backpressure tracking
        if (conn->write_buffer().empty()) {
            // Clear backpressure for this connection
            clear_backpressure_(fd);
            
            // Re-enable read on peer if it was disabled due to backpressure
            if (conn->peer()) {
                if (conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                    reactor_.mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
                    // Clear backpressure for peer as well
                    clear_backpressure_(conn->peer()->fd());
                }
            }
        }
    }
}

void EventHandlers::handle_backend_event(int fd, net::EventType type) {
    auto* conn = get_connection_(fd);
    if (!conn) {
        return;
    }

    // Validate connection state
    if (conn->state() == net::ConnectionState::CLOSED) {
        return;
    }

    // Check backpressure timeout before processing events
    check_backpressure_(fd);

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        // Check if this is a backend connection that can be retried
        if (conn->state() == net::ConnectionState::CONNECTING) {
            // Backend connection error during connect - retry
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_failures();
                    // Metrics: backend failure
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
                }
            }
            
            if (try_retry_on_backend_error(fd)) {
                return;
            }
        }
        
        // For established connections or if retry not possible, just close
        close_connection_(fd);
        return;
    }

    // Check if connection just completed
    if (conn->state() == net::ConnectionState::CONNECTING) {
        // Check for timeout
        auto it_time = connection_times_.find(fd);
        if (it_time != connection_times_.end()) {
            auto elapsed = std::chrono::steady_clock::now() - it_time->second;
            if (elapsed > std::chrono::seconds(connection_timeout_seconds_)) {
                // Connection timeout - retry with next backend
                auto backend_it = backend_connections_.find(fd);
                if (backend_it != backend_connections_.end()) {
                    if (auto backend_node = backend_it->second.lock()) {
                        backend_node->increment_failures();
                        // Metrics: backend failure
                        std::ostringstream backend_str;
                        backend_str << backend_node->host() << ":" << backend_node->port();
                        lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
                    }
                }
                
                if (try_retry_on_backend_error(fd)) {
                    return;
                }
                
                // Fallback: just close if we can't find client
                close_connection_(fd);
                return;
            }
        }
        
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
            // Connection successful
            conn->set_state(net::ConnectionState::ESTABLISHED);
            
            // Increment connection count on backend node
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_connections();
                    // Metrics: backend routed and connection active
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_routed(backend_str.str());
                    lb::metrics::Metrics::instance().increment_connections_active();
                }
            }
            
            // Update epoll to also monitor for reads
            reactor_.mod_fd(fd, EPOLLIN | EPOLLOUT);
            
            // Check if client has pending data to forward
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                // Forward any data that was already read from client while backend was connecting
                if (!conn->peer()->read_buffer().empty()) {
                    forward_data_(conn->peer(), conn);
                }
            }
        } else {
            // Connection failed - check error code and retry
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_failures();
                    // Metrics: backend failure
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
                }
            }
            
            if (try_retry_on_backend_error(fd)) {
                return;
            }
            
            // Fallback: just close if we can't retry
            close_connection_(fd);
            return;
        }
    }

    if (type == net::EventType::READ) {
        // Only read if connection is established
        if (conn->state() != net::ConnectionState::ESTABLISHED) {
            return;
        }

        // Read from backend
        bool read_success = conn->read_from_fd();
        
        // Forward any data that was read (even if EOF was received)
        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
            if (!conn->read_buffer().empty()) {
                forward_data_(conn, conn->peer());
            }
        }
        
        // Only close on error or EOF after forwarding any remaining data
        if (!read_success) {
            // Error or EOF - close connection
            close_connection_(fd);
            return;
        }
    }

    if (type == net::EventType::WRITE) {
        // Only write if connection is established
        if (conn->state() != net::ConnectionState::ESTABLISHED) {
            return;
        }

        // Write to backend
        if (!conn->write_to_fd()) {
            // Error writing
            close_connection_(fd);
            return;
        }

        // If write buffer is empty, clear backpressure tracking
        if (conn->write_buffer().empty()) {
            // Clear backpressure for this connection
            clear_backpressure_(fd);
            
            // Re-enable read on peer if it was disabled due to backpressure
            if (conn->peer()) {
                if (conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                    reactor_.mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
                    // Clear backpressure for peer as well
                    clear_backpressure_(conn->peer()->fd());
                }
            }
        }
    }
}

} // namespace lb::core


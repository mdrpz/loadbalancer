#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include "core/load_balancer.h"
#include "core/backend_node.h"
#include "health/health_checker.h"

namespace lb::core {

LoadBalancer::LoadBalancer() 
    : max_global_connections_(1000), max_connections_per_backend_(100) {
    reactor_ = std::make_unique<net::EpollReactor>();
    backend_pool_ = std::make_unique<BackendPool>();
    health_checker_ = std::make_unique<lb::health::HealthChecker>();
}

LoadBalancer::~LoadBalancer() {
    // Stop health checker
    if (health_checker_) {
        health_checker_->stop();
    }
    
    // Close all connections
    for (auto& [fd, conn] : connections_) {
        if (conn) {
            conn->close();
        }
    }
    connections_.clear();
}

bool LoadBalancer::initialize(const std::string& listen_host, uint16_t listen_port) {
    listener_ = std::make_unique<net::TcpListener>();
    
    if (!listener_->bind(listen_host, listen_port)) {
        return false;
    }
    
    if (!listener_->listen()) {
        return false;
    }
    
    // Register listener fd with reactor for accept events
    int listener_fd = listener_->fd();
    if (!reactor_->add_fd(listener_fd, EPOLLIN, 
                          [this](int fd, net::EventType type) {
                              (void)fd;
                              (void)type;
                              handle_accept();
                          })) {
        return false;
    }
    
    // Start health checker
    if (health_checker_) {
        health_checker_->start();
    }
    
    return true;
}

void LoadBalancer::run() {
    if (reactor_) {
        reactor_->run();
    }
}

void LoadBalancer::stop() {
    // Stop health checker
    if (health_checker_) {
        health_checker_->stop();
    }
    
    // Stop reactor
    if (reactor_) {
        reactor_->stop();
    }
}

void LoadBalancer::add_backend(const std::string& host, uint16_t port) {
    auto backend = std::make_shared<BackendNode>(host, port);
    backend_pool_->add_backend(backend);
    
    // Register with health checker
    if (health_checker_) {
        health_checker_->add_backend(backend);
    }
}

void LoadBalancer::handle_accept() {
    while (true) {
        int client_fd = listener_->accept();
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more connections to accept
            }
            break; // Error accepting
        }

        // Check global connection limit
        // Count established connection pairs (client + backend = 1 connection)
        size_t established_count = count_established_connections();
        if (established_count >= max_global_connections_) {
            // Reject connection - close immediately
            ::close(client_fd);
            continue;
        }

        // Create client connection
        auto client_conn = std::make_unique<net::Connection>(client_fd);
        client_conn->set_state(net::ConnectionState::ESTABLISHED);
        
        // Route to backend
        connect_to_backend(std::move(client_conn));
    }
}

void LoadBalancer::connect_to_backend(std::unique_ptr<net::Connection> client_conn) {
    // Select backend (with connection limit check)
    auto backend_node = backend_pool_->select_backend(max_connections_per_backend_);
    if (!backend_node) {
        // No healthy backends available (all at limit or unhealthy)
        client_conn->close();
        return;
    }

    // Create socket for backend connection
    int backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (backend_fd < 0) {
        client_conn->close();
        return;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(backend_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        // Failed to set socket option, but continue anyway (non-critical)
        // Could log warning here in Phase 2
    }

    // Connect to backend
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend_node->port());
    
    // Validate IP address
    if (inet_pton(AF_INET, backend_node->host().c_str(), &addr.sin_addr) <= 0) {
        // Invalid IP address
        backend_node->increment_failures();
        client_conn->close();
        ::close(backend_fd);
        return;
    }

    int result = connect(backend_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    
    // Create backend connection
    auto backend_conn = std::make_unique<net::Connection>(backend_fd);
    
    if (result < 0 && errno != EINPROGRESS) {
        // Connection failed immediately
        backend_node->increment_failures();
        client_conn->close();
        backend_conn->close();
        return;
    }

    // Cross-wire connections
    client_conn->set_peer(backend_conn.get());
    backend_conn->set_peer(client_conn.get());

    if (result == 0) {
        // Connected immediately
        backend_conn->set_state(net::ConnectionState::ESTABLISHED);
        backend_node->increment_connections();
    } else {
        // Connection in progress
        backend_conn->set_state(net::ConnectionState::CONNECTING);
    }

    // Store connections
    int client_fd = client_conn->fd();
    int backend_fd_stored = backend_conn->fd();
    connections_[client_fd] = std::move(client_conn);
    connections_[backend_fd_stored] = std::move(backend_conn);
    
    // Track backend connection and start time
    backend_connections_[backend_fd_stored] = backend_node;
    connection_times_[backend_fd_stored] = std::chrono::steady_clock::now();

    // Register client with reactor (level-triggered, default)
    // Monitor both read and write events
    reactor_->add_fd(client_fd, EPOLLIN | EPOLLOUT,
                     [this](int fd, net::EventType type) {
                         handle_client_event(fd, type);
                     });

    // Register backend with reactor (level-triggered)
    uint32_t events = EPOLLOUT; // Monitor for connection completion
    if (connections_[backend_fd_stored]->state() == net::ConnectionState::ESTABLISHED) {
        events |= EPOLLIN; // Also monitor for read if already connected
    }
    reactor_->add_fd(backend_fd_stored, events,
                     [this](int fd, net::EventType type) {
                         handle_backend_event(fd, type);
                     });
}

void LoadBalancer::handle_client_event(int fd, net::EventType type) {
    auto* conn = get_connection(fd);
    if (!conn) {
        return;
    }

    // Validate connection state
    if (conn->state() == net::ConnectionState::CLOSED) {
        return;
    }

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection(fd);
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
                forward_data(conn, conn->peer());
            }
        }
        
        // Handle EOF from client - shutdown write side but keep connection open to receive backend response
        if (!read_success) {
            // Forward any remaining data in read buffer before shutting down
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                if (!conn->read_buffer().empty()) {
                    forward_data(conn, conn->peer());
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
            close_connection(fd);
            return;
        }

        // If write buffer is empty, we can read more from peer
        if (conn->write_buffer().empty() && conn->peer()) {
            // Re-enable read on peer if it was disabled due to backpressure
            if (conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                reactor_->mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
            }
        }
    }
}

void LoadBalancer::handle_backend_event(int fd, net::EventType type) {
    auto* conn = get_connection(fd);
    if (!conn) {
        return;
    }

    // Validate connection state
    if (conn->state() == net::ConnectionState::CLOSED) {
        return;
    }

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection(fd);
        return;
    }

    // Check if connection just completed
    if (conn->state() == net::ConnectionState::CONNECTING) {
        // Check for timeout (5 seconds default per plan)
        auto it_time = connection_times_.find(fd);
        if (it_time != connection_times_.end()) {
            auto elapsed = std::chrono::steady_clock::now() - it_time->second;
            if (elapsed > std::chrono::seconds(5)) {
                // Connection timeout
                auto backend_it = backend_connections_.find(fd);
                if (backend_it != backend_connections_.end()) {
                    if (auto backend_node = backend_it->second.lock()) {
                        backend_node->increment_failures();
                    }
                }
                close_connection(fd);
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
                }
            }
            
            // Update epoll to also monitor for reads
            reactor_->mod_fd(fd, EPOLLIN | EPOLLOUT);
            
            // Check if client has pending data to forward
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                // Forward any data that was already read from client while backend was connecting
                if (!conn->peer()->read_buffer().empty()) {
                    forward_data(conn->peer(), conn);
                }
            }
        } else {
            // Connection failed
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_failures();
                }
            }
            close_connection(fd);
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
                forward_data(conn, conn->peer());
            }
        }
        
        // Only close on error or EOF after forwarding any remaining data
        if (!read_success) {
            // Error or EOF - close connection
            close_connection(fd);
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
            close_connection(fd);
            return;
        }

        // If write buffer is empty, we can read more from peer
        if (conn->write_buffer().empty() && conn->peer()) {
            // Re-enable read on peer if it was disabled due to backpressure
            if (conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                reactor_->mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
            }
        }
    }
}

void LoadBalancer::forward_data(net::Connection* from, net::Connection* to) {
    if (!from || !to) {
        return;
    }

    // Validate both connections are established
    if (from->state() != net::ConnectionState::ESTABLISHED ||
        to->state() != net::ConnectionState::ESTABLISHED) {
        return;
    }

    // Copy data from from->read_buf to to->write_buf
    auto& read_buf = from->read_buffer();
    auto& write_buf = to->write_buffer();

    if (read_buf.empty()) {
        return;
    }

    // Check if destination buffer has space
    size_t available = to->write_available();
    if (available == 0) {
        // Destination buffer full - stop reading from source
        // Only disable reads, keep writes enabled
        reactor_->mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    // Copy data
    size_t to_copy = std::min(read_buf.size(), available);
    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    // Enable write events on destination
    reactor_->mod_fd(to->fd(), EPOLLIN | EPOLLOUT);

    // Re-enable reads on source if we had disabled them due to backpressure
    // and now there's space in destination
    reactor_->mod_fd(from->fd(), EPOLLIN | EPOLLOUT);

    // Try to write immediately
    if (!to->write_to_fd()) {
        close_connection(to->fd());
        return;
    }
}

void LoadBalancer::close_connection(int fd) {
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
                
                // Remove from reactor (safe even if already removed)
                reactor_->del_fd(peer_fd);
                
                // Close and remove peer connection
                peer_conn->close();
                connections_.erase(peer_it);
            }
        }
    }

    // Remove from reactor (safe even if already removed)
    reactor_->del_fd(fd);
    
    // Close connection and remove from map
    conn->close();
    connections_.erase(it);
}

net::Connection* LoadBalancer::get_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return nullptr;
    }
    return it->second.get();
}

size_t LoadBalancer::count_established_connections() const {
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

} // namespace lb::core


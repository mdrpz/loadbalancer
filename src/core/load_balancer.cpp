#include "core/load_balancer.h"
#include "core/backend_node.h"
#include <stdexcept>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <cstring>

namespace lb::core {

LoadBalancer::LoadBalancer() {
    reactor_ = std::make_unique<net::EpollReactor>();
    backend_pool_ = std::make_unique<BackendPool>();
}

LoadBalancer::~LoadBalancer() {
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
    
    return true;
}

void LoadBalancer::run() {
    if (reactor_) {
        reactor_->run();
    }
}

void LoadBalancer::stop() {
    if (reactor_) {
        reactor_->stop();
    }
}

void LoadBalancer::add_backend(const std::string& host, uint16_t port) {
    auto backend = std::make_shared<BackendNode>(host, port);
    backend_pool_->add_backend(backend);
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

        // Create client connection
        auto client_conn = std::make_unique<net::Connection>(client_fd);
        client_conn->set_state(net::ConnectionState::ESTABLISHED);
        
        // Route to backend
        connect_to_backend(std::move(client_conn));
    }
}

void LoadBalancer::connect_to_backend(std::unique_ptr<net::Connection> client_conn) {
    // Select backend
    auto backend_node = backend_pool_->select_backend();
    if (!backend_node) {
        // No healthy backends available
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
    setsockopt(backend_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Connect to backend
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend_node->port());
    inet_pton(AF_INET, backend_node->host().c_str(), &addr.sin_addr);

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

    // Register client with reactor (level-triggered, default)
    reactor_->add_fd(client_fd, EPOLLIN,
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

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection(fd);
        return;
    }

    if (type == net::EventType::READ) {
        // Read from client
        if (!conn->read_from_fd()) {
            // Error or EOF
            close_connection(fd);
            return;
        }

        // Forward to backend
        if (conn->peer()) {
            forward_data(conn, conn->peer());
        }
    }
}

void LoadBalancer::handle_backend_event(int fd, net::EventType type) {
    auto* conn = get_connection(fd);
    if (!conn) {
        return;
    }

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection(fd);
        return;
    }

    // Check if connection just completed
    if (conn->state() == net::ConnectionState::CONNECTING) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
            // Connection successful
            conn->set_state(net::ConnectionState::ESTABLISHED);
            // Find backend node and increment connection count
            // (We'll need to track this better in Phase 2)
            // Update epoll to also monitor for reads
            reactor_->mod_fd(fd, EPOLLIN | EPOLLOUT);
        } else {
            // Connection failed
            close_connection(fd);
            return;
        }
    }

    if (type == net::EventType::READ) {
        // Read from backend
        if (!conn->read_from_fd()) {
            // Error or EOF
            close_connection(fd);
            return;
        }

        // Forward to client
        if (conn->peer()) {
            forward_data(conn, conn->peer());
        }
    }

    if (type == net::EventType::WRITE) {
        // Write to backend
        if (!conn->write_to_fd()) {
            // Error writing
            close_connection(fd);
            return;
        }

        // If write buffer is empty, we can read more from peer
        if (conn->write_buffer().empty() && conn->peer()) {
            // Re-enable read on peer if it was disabled
            reactor_->mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
        }
    }
}

void LoadBalancer::forward_data(net::Connection* from, net::Connection* to) {
    if (!from || !to) {
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
        reactor_->mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    // Copy data
    size_t to_copy = std::min(read_buf.size(), available);
    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    // Enable write events on destination
    reactor_->mod_fd(to->fd(), EPOLLIN | EPOLLOUT);

    // Try to write immediately
    if (!to->write_to_fd()) {
        close_connection(to->fd());
        return;
    }
}

void LoadBalancer::close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;
    }

    auto* conn = it->second.get();
    if (conn && conn->peer()) {
        // Close peer connection too
        int peer_fd = conn->peer()->fd();
        reactor_->del_fd(peer_fd);
        connections_.erase(peer_fd);
    }

    reactor_->del_fd(fd);
    connections_.erase(it);
}

net::Connection* LoadBalancer::get_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return nullptr;
    }
    return it->second.get();
}

} // namespace lb::core


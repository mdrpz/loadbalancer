#include "core/backend_connector.h"
#include "metrics/metrics.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <chrono>
#include <sstream>

namespace lb::core {

BackendConnector::BackendConnector(
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
    std::unordered_map<int, int>& backend_to_client_map,
    std::unordered_map<int, int>& client_retry_counts,
    BackendPool& backend_pool,
    net::EpollReactor& reactor,
    uint32_t max_connections_per_backend,
    std::function<void(int, net::EventType)> client_handler,
    std::function<void(int, net::EventType)> backend_handler,
    std::function<void(std::unique_ptr<net::Connection>, int)> retry_callback)
    : connections_(connections),
      backend_connections_(backend_connections),
      connection_times_(connection_times),
      backend_to_client_map_(backend_to_client_map),
      client_retry_counts_(client_retry_counts),
      backend_pool_(backend_pool),
      reactor_(reactor),
      max_connections_per_backend_(max_connections_per_backend),
      client_handler_(client_handler),
      backend_handler_(backend_handler),
      retry_callback_(retry_callback) {
}

void BackendConnector::connect(std::unique_ptr<net::Connection> client_conn, int retry_count) {
    // Select backend (with connection limit check)
    auto backend_node = backend_pool_.select_backend(max_connections_per_backend_);
    if (!backend_node) {
        // No healthy backends available (all at limit or unhealthy)
        lb::metrics::Metrics::instance().increment_backend_routes_failed();
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
        std::ostringstream backend_str;
        backend_str << backend_node->host() << ":" << backend_node->port();
        lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
        client_conn->close();
        ::close(backend_fd);
        return;
    }

    int result = ::connect(backend_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    
    // Create backend connection
    auto backend_conn = std::make_unique<net::Connection>(backend_fd);
    
    if (result < 0 && errno != EINPROGRESS) {
        // Connection failed immediately - retry with next backend
        backend_node->increment_failures();
        std::ostringstream backend_str;
        backend_str << backend_node->host() << ":" << backend_node->port();
        lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
        backend_conn->close();
        ::close(backend_fd);
        
        // Retry with next backend
        retry_callback_(std::move(client_conn), retry_count);
        return;
    }

    // Cross-wire connections
    client_conn->set_peer(backend_conn.get());
    backend_conn->set_peer(client_conn.get());

    std::ostringstream backend_str;
    backend_str << backend_node->host() << ":" << backend_node->port();
    std::string backend_key = backend_str.str();
    
    if (result == 0) {
        // Connected immediately
        backend_conn->set_state(net::ConnectionState::ESTABLISHED);
        backend_node->increment_connections();
        // Metrics: backend routed and connection active
        lb::metrics::Metrics::instance().increment_backend_routed(backend_key);
        lb::metrics::Metrics::instance().increment_connections_active();
    } else {
        // Connection in progress - will be tracked when connection completes
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
    
    // Track client connection for retry purposes
    backend_to_client_map_[backend_fd_stored] = client_fd;
    client_retry_counts_[client_fd] = retry_count;

    // Register client with reactor (level-triggered, default)
    // Monitor both read and write events
    reactor_.add_fd(client_fd, EPOLLIN | EPOLLOUT,
                    [this](int fd, net::EventType type) {
                        client_handler_(fd, type);
                    });

    // Register backend with reactor (level-triggered)
    uint32_t events = EPOLLOUT; // Monitor for connection completion
    if (connections_[backend_fd_stored]->state() == net::ConnectionState::ESTABLISHED) {
        events |= EPOLLIN; // Also monitor for read if already connected
    }
    reactor_.add_fd(backend_fd_stored, events,
                    [this](int fd, net::EventType type) {
                        backend_handler_(fd, type);
                    });
}

} // namespace lb::core


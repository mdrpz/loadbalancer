#include "core/backend_connector.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>
#include "logging/logger.h"
#include "metrics/metrics.h"

namespace lb::core {

BackendConnector::BackendConnector(
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
    std::unordered_map<int, int>& backend_to_client_map,
    std::unordered_map<int, int>& client_retry_counts, BackendPool& backend_pool,
    net::EpollReactor& reactor, uint32_t max_connections_per_backend,
    std::function<void(int, net::EventType)> client_handler,
    std::function<void(int, net::EventType)> backend_handler,
    std::function<void(std::unique_ptr<net::Connection>, int)> retry_callback)
    : connections_(connections), backend_connections_(backend_connections),
      connection_times_(connection_times), backend_to_client_map_(backend_to_client_map),
      client_retry_counts_(client_retry_counts), backend_pool_(backend_pool), reactor_(reactor),
      max_connections_per_backend_(max_connections_per_backend),
      client_handler_(std::move(std::move(client_handler))),
      backend_handler_(std::move(std::move(backend_handler))),
      retry_callback_(std::move(std::move(retry_callback))) {}

void BackendConnector::set_pool_manager(ConnectionPoolManager* pool_manager) {
    pool_manager_ = pool_manager;
}

BackendConnector::BackendInfo BackendConnector::get_backend_info(int backend_fd) const {
    auto it = pooled_connections_.find(backend_fd);
    if (it != pooled_connections_.end()) {
        return it->second;
    }
    return {"", 0, false};
}

void BackendConnector::connect(std::unique_ptr<net::Connection> client_conn, int retry_count) {
    auto backend_node = backend_pool_.select_backend(max_connections_per_backend_);
    if (!backend_node) {
        lb::metrics::Metrics::instance().increment_backend_routes_failed();
        lb::logging::Logger::instance().warn("No healthy backend available for routing");
        client_conn->close();
        return;
    }

    const std::string& host = backend_node->host();
    uint16_t port = backend_node->port();

    lb::logging::Logger::instance().debug("Routing connection to backend " + host + ":" +
                                          std::to_string(port));

    std::ostringstream backend_str;
    backend_str << host << ":" << port;
    std::string backend_key = backend_str.str();

    if (pool_manager_) {
        auto pooled_backend = pool_manager_->borrow(host, port);
        if (pooled_backend) {
            lb::logging::Logger::instance().debug("Using pooled connection to " + backend_key);

            int backend_fd = pooled_backend->fd();

            client_conn->set_peer(pooled_backend.get());
            pooled_backend->set_peer(client_conn.get());

            pooled_connections_[backend_fd] = {host, port, true};

            backend_node->increment_connections();
            lb::metrics::Metrics::instance().increment_backend_routed(backend_key);
            lb::metrics::Metrics::instance().increment_connections_active();

            int client_fd = client_conn->fd();
            connections_[client_fd] = std::move(client_conn);
            connections_[backend_fd] = std::move(pooled_backend);

            backend_connections_[backend_fd] = backend_node;
            connection_times_[backend_fd] = std::chrono::steady_clock::now();
            backend_to_client_map_[backend_fd] = client_fd;
            client_retry_counts_[client_fd] = retry_count;

            reactor_.add_fd(client_fd, EPOLLIN | EPOLLOUT,
                            [this](int fd, net::EventType type) { client_handler_(fd, type); });
            reactor_.add_fd(backend_fd, EPOLLIN | EPOLLOUT,
                            [this](int fd, net::EventType type) { backend_handler_(fd, type); });
            return;
        }
    }

    int backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (backend_fd < 0) {
        client_conn->close();
        return;
    }

    int opt = 1;
    if (setsockopt(backend_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket option: " << strerror(errno) << std::endl;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        backend_node->increment_failures();
        lb::metrics::Metrics::instance().increment_backend_failures(backend_key);
        client_conn->close();
        ::close(backend_fd);
        return;
    }

    int result = ::connect(backend_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    auto backend_conn = std::make_unique<net::Connection>(backend_fd);

    if (result < 0 && errno != EINPROGRESS) {
        backend_node->increment_failures();
        lb::metrics::Metrics::instance().increment_backend_failures(backend_key);
        backend_conn->close();
        ::close(backend_fd);

        retry_callback_(std::move(client_conn), retry_count);
        return;
    }

    client_conn->set_peer(backend_conn.get());
    backend_conn->set_peer(client_conn.get());

    pooled_connections_[backend_fd] = {host, port, false};

    if (result == 0) {
        backend_conn->set_state(net::ConnectionState::ESTABLISHED);
        backend_node->increment_connections();
        lb::metrics::Metrics::instance().increment_backend_routed(backend_key);
        lb::metrics::Metrics::instance().increment_connections_active();
        lb::logging::Logger::instance().debug("Backend connection established to " + backend_key);
    } else {
        backend_conn->set_state(net::ConnectionState::CONNECTING);
        lb::logging::Logger::instance().debug("Backend connection in progress to " + backend_key);
    }

    int client_fd = client_conn->fd();
    int backend_fd_stored = backend_conn->fd();
    connections_[client_fd] = std::move(client_conn);
    connections_[backend_fd_stored] = std::move(backend_conn);

    backend_connections_[backend_fd_stored] = backend_node;
    connection_times_[backend_fd_stored] = std::chrono::steady_clock::now();

    backend_to_client_map_[backend_fd_stored] = client_fd;
    client_retry_counts_[client_fd] = retry_count;

    reactor_.add_fd(client_fd, EPOLLIN | EPOLLOUT,
                    [this](int fd, net::EventType type) { client_handler_(fd, type); });

    uint32_t events = EPOLLOUT;
    if (connections_[backend_fd_stored]->state() == net::ConnectionState::ESTABLISHED)
        events |= EPOLLIN;
    reactor_.add_fd(backend_fd_stored, events,
                    [this](int fd, net::EventType type) { backend_handler_(fd, type); });
}

} // namespace lb::core

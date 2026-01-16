#include "core/connection_pool.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <utility>
#include "logging/logger.h"

namespace lb::core {

ConnectionPool::ConnectionPool(std::string host, uint16_t port, const PoolConfig& config)
    : host_(std::move(host)), port_(port), config_(config) {}

ConnectionPool::~ConnectionPool() {
    clear();
}

std::unique_ptr<net::Connection> ConnectionPool::borrow() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        auto& pc = *it;
        if (!pc->in_use && pc->connection) {
            if (config_.validate_on_borrow && !validate_connection(pc->connection.get())) {
                close_connection(pc.get());
                connections_.erase(it);
                continue;
            }
            auto conn = std::move(pc->connection);
            connections_.erase(it);
            active_count_++;
            lb::logging::Logger::instance().debug("Pool: Borrowed existing connection to " + host_ +
                                                  ":" + std::to_string(port_));
            return conn;
        }
    }

    if (active_count_ < config_.max_connections) {
        auto* raw_conn = create_connection();
        if (raw_conn) {
            active_count_++;
            lb::logging::Logger::instance().debug(
                "Pool: Created new connection to " + host_ + ":" + std::to_string(port_) +
                " (active: " + std::to_string(active_count_) + ")");
            return std::unique_ptr<net::Connection>(raw_conn);
        }
    }

    lb::logging::Logger::instance().warn("Pool: No available connections to " + host_ + ":" +
                                         std::to_string(port_) +
                                         " (max: " + std::to_string(config_.max_connections) +
                                         ", active: " + std::to_string(active_count_) + ")");
    return nullptr;
}

void ConnectionPool::release(std::unique_ptr<net::Connection> conn) {
    if (!conn)
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (active_count_ > 0)
        active_count_--;

    if (!validate_connection(conn.get())) {
        lb::logging::Logger::instance().debug(
            "Pool: Connection invalid, closing instead of returning to pool");
        conn->close();
        return;
    }

    auto pc = std::make_unique<PooledConnection>();
    pc->connection = std::move(conn);
    pc->last_used = std::chrono::steady_clock::now();
    pc->in_use = false;
    connections_.push_back(std::move(pc));

    lb::logging::Logger::instance().debug("Pool: Released connection to " + host_ + ":" +
                                          std::to_string(port_) +
                                          " (idle: " + std::to_string(connections_.size()) + ")");
}

void ConnectionPool::invalidate(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_count_ > 0)
        active_count_--;
    lb::logging::Logger::instance().debug("Pool: Invalidated connection fd=" + std::to_string(fd) +
                                          " to " + host_ + ":" + std::to_string(port_));
}

void ConnectionPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pc : connections_) {
        close_connection(pc.get());
    }
    connections_.clear();
    active_count_ = 0;

    lb::logging::Logger::instance().debug("Pool: Cleared all connections to " + host_ + ":" +
                                          std::to_string(port_));
}

size_t ConnectionPool::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_count_;
}

size_t ConnectionPool::idle_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& pc : connections_) {
        if (!pc->in_use && pc->connection)
            count++;
    }
    return count;
}

size_t ConnectionPool::total_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

void ConnectionPool::evict_expired() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    size_t idle_count = 0;

    for (const auto& pc : connections_) {
        if (!pc->in_use)
            idle_count++;
    }

    auto it = connections_.begin();
    while (it != connections_.end()) {
        auto& pc = *it;

        if (!pc->in_use && idle_count > config_.min_connections) {
            auto idle_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - pc->last_used).count();

            if (static_cast<size_t>(idle_time) > config_.max_idle_time_ms) {
                close_connection(pc.get());
                it = connections_.erase(it);
                idle_count--;
                lb::logging::Logger::instance().debug("Pool: Evicted expired connection to " +
                                                      host_ + ":" + std::to_string(port_));
                continue;
            }
        }
        ++it;
    }
}

net::Connection* ConnectionPool::create_connection() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        lb::logging::Logger::instance().error("Pool: Failed to create socket: " +
                                              std::string(strerror(errno)));
        return nullptr;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

    struct addrinfo hints {
    }, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res);
    if (gai_err != 0 || !res) {
        lb::logging::Logger::instance().error("Pool: Failed to resolve " + host_ + ": " +
                                              gai_strerror(gai_err));
        ::close(fd);
        return nullptr;
    }

    int result = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (result < 0 && errno != EINPROGRESS) {
        lb::logging::Logger::instance().error("Pool: Connect failed to " + host_ + ":" +
                                              std::to_string(port_) + ": " + strerror(errno));
        ::close(fd);
        return nullptr;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

    result = select(fd + 1, nullptr, &write_fds, nullptr, &tv);
    if (result <= 0) {
        lb::logging::Logger::instance().error("Pool: Connect timeout to " + host_ + ":" +
                                              std::to_string(port_));
        ::close(fd);
        return nullptr;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        lb::logging::Logger::instance().error("Pool: Connect error to " + host_ + ":" +
                                              std::to_string(port_) + ": " + strerror(error));
        ::close(fd);
        return nullptr;
    }

    auto* conn = new net::Connection(fd);
    conn->set_state(net::ConnectionState::ESTABLISHED);
    return conn;
}

bool ConnectionPool::validate_connection(net::Connection* conn) {
    if (!conn || conn->fd() < 0)
        return false;
    if (conn->state() != net::ConnectionState::ESTABLISHED)
        return false;

    char buf;
    int result = recv(conn->fd(), &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
        return false;
    }
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return false;
    }

    return true;
}

void ConnectionPool::close_connection(PooledConnection* pc) {
    if (pc && pc->connection) {
        pc->connection->close();
        pc->connection.reset();
    }
}

ConnectionPoolManager::ConnectionPoolManager(const PoolConfig& default_config)
    : default_config_(default_config) {}

std::string ConnectionPoolManager::make_key(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

ConnectionPool* ConnectionPoolManager::get_pool(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = make_key(host, port);
    auto it = pools_.find(key);
    if (it != pools_.end()) {
        return it->second.get();
    }

    auto pool = std::make_unique<ConnectionPool>(host, port, default_config_);
    ConnectionPool* result = pool.get();
    pools_[key] = std::move(pool);

    lb::logging::Logger::instance().info("Pool: Created pool for " + host + ":" +
                                         std::to_string(port));
    return result;
}

std::unique_ptr<net::Connection> ConnectionPoolManager::borrow(const std::string& host,
                                                               uint16_t port) {
    ConnectionPool* pool = get_pool(host, port);
    return pool ? pool->borrow() : nullptr;
}

void ConnectionPoolManager::release(const std::string& host, uint16_t port,
                                    std::unique_ptr<net::Connection> conn) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = make_key(host, port);
    auto it = pools_.find(key);
    if (it != pools_.end()) {
        it->second->release(std::move(conn));
    }
}

void ConnectionPoolManager::invalidate(const std::string& host, uint16_t port, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = make_key(host, port);
    auto it = pools_.find(key);
    if (it != pools_.end()) {
        it->second->invalidate(fd);
    }
}

void ConnectionPoolManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, pool] : pools_) {
        pool->clear();
    }
    pools_.clear();
}

void ConnectionPoolManager::evict_expired() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, pool] : pools_) {
        pool->evict_expired();
    }
}

} // namespace lb::core

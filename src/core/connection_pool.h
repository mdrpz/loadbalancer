#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "net/connection.h"

namespace lb::core {

struct PoolConfig {
    size_t min_connections = 0;
    size_t max_connections = 10;
    size_t max_idle_time_ms = 60000;
    size_t connect_timeout_ms = 5000;
    bool validate_on_borrow = true;
};

struct PooledConnection {
    std::unique_ptr<net::Connection> connection;
    std::chrono::steady_clock::time_point last_used;
    bool in_use = false;
};

class ConnectionPool {
public:
    ConnectionPool(std::string host, uint16_t port, const PoolConfig& config);
    ~ConnectionPool();

    std::unique_ptr<net::Connection> borrow();

    void release(std::unique_ptr<net::Connection> conn);

    void invalidate(int fd);

    void clear();

    size_t active_count() const;
    size_t idle_count() const;
    size_t total_count() const;

    void evict_expired();

    const std::string& host() const {
        return host_;
    }
    uint16_t port() const {
        return port_;
    }

private:
    net::Connection* create_connection();
    static bool validate_connection(net::Connection* conn);
    static void close_connection(PooledConnection* pc);

    std::string host_;
    uint16_t port_;
    PoolConfig config_;

    mutable std::mutex mutex_;
    std::deque<std::unique_ptr<PooledConnection>> connections_;
    size_t active_count_ = 0;
};

class ConnectionPoolManager {
public:
    ConnectionPoolManager(const PoolConfig& default_config = PoolConfig{});

    ConnectionPool* get_pool(const std::string& host, uint16_t port);

    std::unique_ptr<net::Connection> borrow(const std::string& host, uint16_t port);

    void release(const std::string& host, uint16_t port, std::unique_ptr<net::Connection> conn);

    void invalidate(const std::string& host, uint16_t port, int fd);

    void clear();

    void evict_expired();

    void set_config(const PoolConfig& config) {
        default_config_ = config;
    }

private:
    static std::string make_key(const std::string& host, uint16_t port);

    PoolConfig default_config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ConnectionPool>> pools_;
};

} // namespace lb::core

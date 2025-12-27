#pragma once

#include <string>
#include <cstdint>
#include <atomic>

namespace lb::core {

enum class BackendState {
    HEALTHY,
    UNHEALTHY,
    DRAINING
};

class BackendNode {
public:
    BackendNode(const std::string& host, uint16_t port);
    
    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }
    
    BackendState state() const { return state_.load(); }
    void set_state(BackendState state) { state_.store(state); }
    
    uint32_t active_connections() const { return active_connections_.load(); }
    void increment_connections() { active_connections_++; }
    void decrement_connections() { active_connections_--; }
    
    uint32_t failure_count() const { return failure_count_.load(); }
    void increment_failures() { failure_count_++; }
    void reset_failures() { failure_count_ = 0; }

private:
    std::string host_;
    uint16_t port_;
    std::atomic<BackendState> state_;
    std::atomic<uint32_t> active_connections_;
    std::atomic<uint32_t> failure_count_;
};

} // namespace lb::core


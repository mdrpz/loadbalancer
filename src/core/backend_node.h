#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace lb::core {

enum class BackendState { HEALTHY, UNHEALTHY, DRAINING };

class BackendNode {
public:
    BackendNode(std::string host, uint16_t port, uint32_t weight = 1);

    [[nodiscard]] const std::string& host() const {
        return host_;
    }
    [[nodiscard]] uint16_t port() const {
        return port_;
    }
    [[nodiscard]] uint32_t weight() const {
        return weight_;
    }

    [[nodiscard]] BackendState state() const {
        return state_.load();
    }
    void set_state(BackendState state) {
        state_.store(state);
    }

    [[nodiscard]] uint32_t active_connections() const {
        return active_connections_.load();
    }
    void increment_connections() {
        active_connections_++;
    }
    void decrement_connections() {
        active_connections_--;
    }

    [[nodiscard]] uint32_t failure_count() const {
        return failure_count_.load();
    }
    void increment_failures() {
        failure_count_++;
    }
    void reset_failures() {
        failure_count_ = 0;
    }

private:
    std::string host_;
    uint16_t port_;
    uint32_t weight_;
    std::atomic<BackendState> state_;
    std::atomic<uint32_t> active_connections_;
    std::atomic<uint32_t> failure_count_;
};

} // namespace lb::core

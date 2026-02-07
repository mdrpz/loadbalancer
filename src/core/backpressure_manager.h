#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace lb::core {

class BackpressureManager {
public:
    BackpressureManager(
        std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
        uint32_t timeout_ms);

    void check_timeout(int fd, const std::function<void(int)>& close_callback);
    bool start_tracking(int fd);
    bool clear_tracking(int fd);

private:
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_start_times_;
    uint32_t timeout_ms_;
};

} // namespace lb::core

#pragma once

#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <functional>

namespace lb::core {

class BackpressureManager {
public:
    BackpressureManager(std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
                       uint32_t timeout_ms);
    
    void check_timeout(int fd, std::function<void(int)> close_callback);
    void start_tracking(int fd);
    void clear_tracking(int fd);

private:
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_start_times_;
    uint32_t timeout_ms_;
};

} // namespace lb::core


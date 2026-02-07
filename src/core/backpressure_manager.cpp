#include "core/backpressure_manager.h"
#include <chrono>
#include <functional>

namespace lb::core {

BackpressureManager::BackpressureManager(
    std::unordered_map<int, std::chrono::steady_clock::time_point>& backpressure_times,
    uint32_t timeout_ms)
    : backpressure_start_times_(backpressure_times), timeout_ms_(timeout_ms) {}

void BackpressureManager::check_timeout(int fd, const std::function<void(int)>& close_callback) {
    auto it = backpressure_start_times_.find(fd);
    if (it == backpressure_start_times_.end())
        return;

    auto elapsed = std::chrono::steady_clock::now() - it->second;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    if (elapsed_ms >= static_cast<int64_t>(timeout_ms_))
        close_callback(fd);
}

bool BackpressureManager::start_tracking(int fd) {
    auto it = backpressure_start_times_.find(fd);
    if (it == backpressure_start_times_.end()) {
        backpressure_start_times_[fd] = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

bool BackpressureManager::clear_tracking(int fd) {
    auto erased = backpressure_start_times_.erase(fd);
    return erased > 0;
}

} // namespace lb::core

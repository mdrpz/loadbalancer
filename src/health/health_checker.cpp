#include "health/health_checker.h"
#include <chrono>
#include <thread>

namespace lb::health {

HealthChecker::HealthChecker()
    : running_(false), interval_ms_(5000), timeout_ms_(500),
      failure_threshold_(3), success_threshold_(2) {
}

HealthChecker::~HealthChecker() {
    stop();
}

void HealthChecker::add_backend(std::shared_ptr<lb::core::BackendNode> backend) {
    // TODO: Thread-safe add
    backends_.push_back(backend);
}

void HealthChecker::remove_backend(std::shared_ptr<lb::core::BackendNode> backend) {
    // TODO: Thread-safe remove
    backends_.erase(
        std::remove(backends_.begin(), backends_.end(), backend),
        backends_.end()
    );
}

void HealthChecker::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }
    thread_ = std::thread(&HealthChecker::run_loop, this);
}

void HealthChecker::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void HealthChecker::run_loop() {
    while (running_) {
        for (auto& backend : backends_) {
            if (!running_) break;
            check_backend(backend);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

bool HealthChecker::check_backend(std::shared_ptr<lb::core::BackendNode> backend) {
    // TODO: Implement TCP connect health check (Phase 2)
    // TODO: Implement HTTP health check option (Phase 2)
    (void)backend;
    return false;
}

} // namespace lb::health


#pragma once

#include "core/backend_node.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace lb::health {

class HealthChecker {
public:
    HealthChecker();
    ~HealthChecker();

    void add_backend(std::shared_ptr<lb::core::BackendNode> backend);
    void remove_backend(std::shared_ptr<lb::core::BackendNode> backend);
    
    void start();
    void stop();

private:
    void run_loop();
    bool check_backend(std::shared_ptr<lb::core::BackendNode> backend);
    void update_backend_state(std::shared_ptr<lb::core::BackendNode> backend, bool healthy);

    std::vector<std::shared_ptr<lb::core::BackendNode>> backends_;
    std::mutex backends_mutex_;
    std::thread thread_;
    std::atomic<bool> running_;
    uint32_t interval_ms_;
    uint32_t timeout_ms_;
    uint32_t failure_threshold_;
    uint32_t success_threshold_;
    
    // Track consecutive failures/successes per backend
    std::unordered_map<std::shared_ptr<lb::core::BackendNode>, uint32_t> consecutive_failures_;
    std::unordered_map<std::shared_ptr<lb::core::BackendNode>, uint32_t> consecutive_successes_;
    std::mutex state_mutex_;
};

} // namespace lb::health


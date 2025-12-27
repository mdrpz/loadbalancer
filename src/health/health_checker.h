#pragma once

#include "core/backend_node.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>

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

    std::vector<std::shared_ptr<lb::core::BackendNode>> backends_;
    std::thread thread_;
    std::atomic<bool> running_;
    uint32_t interval_ms_;
    uint32_t timeout_ms_;
    uint32_t failure_threshold_;
    uint32_t success_threshold_;
};

} // namespace lb::health


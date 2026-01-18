#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "core/backend_node.h"

namespace lb::health {

class HealthChecker {
public:
    HealthChecker();
    ~HealthChecker();

    void add_backend(const std::shared_ptr<lb::core::BackendNode>& backend);
    void remove_backend(const std::shared_ptr<lb::core::BackendNode>& backend);

    void configure(uint32_t interval_ms, uint32_t timeout_ms, uint32_t failure_threshold,
                   uint32_t success_threshold, const std::string& type);

    void start();
    void stop();

private:
    void run_loop();
    bool check_backend(const std::shared_ptr<lb::core::BackendNode>& backend);
    void update_backend_state(const std::shared_ptr<lb::core::BackendNode>& backend, bool healthy);

    std::vector<std::shared_ptr<lb::core::BackendNode>> backends_;
    std::mutex backends_mutex_;
    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<uint32_t> interval_ms_;
    std::atomic<uint32_t> timeout_ms_;
    std::atomic<uint32_t> failure_threshold_;
    std::atomic<uint32_t> success_threshold_;
    std::string health_check_type_;
    std::mutex config_mutex_;

    std::unordered_map<std::shared_ptr<lb::core::BackendNode>, uint32_t> consecutive_failures_;
    std::unordered_map<std::shared_ptr<lb::core::BackendNode>, uint32_t> consecutive_successes_;
    std::mutex state_mutex_;
};

} // namespace lb::health

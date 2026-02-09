#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace lb::core {
class ThreadPool;
} // namespace lb::core

namespace lb::metrics {

class MetricsServer {
public:
    MetricsServer(uint16_t port);
    ~MetricsServer();

    void start();
    void stop();

    void set_thread_pool(lb::core::ThreadPool* pool);

private:
    void run_loop();
    static void handle_request(int client_fd);
    static void send_response(int client_fd, const std::string& body, bool is_metrics);

    uint16_t port_;
    int listener_fd_;
    std::thread thread_;
    std::atomic<bool> running_;

    lb::core::ThreadPool* thread_pool_{nullptr};
    std::atomic<bool> pool_task_active_{false};
};

} // namespace lb::metrics

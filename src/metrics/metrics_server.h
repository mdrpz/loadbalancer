#pragma once

#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>

namespace lb::metrics {

class MetricsServer {
public:
    MetricsServer(uint16_t port);
    ~MetricsServer();

    void start();
    void stop();

private:
    void run_loop();
    
    uint16_t port_;
    int listener_fd_;
    std::thread thread_;
    std::atomic<bool> running_;
};

} // namespace lb::metrics


#pragma once

#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <string>

namespace lb::metrics {

class MetricsServer {
public:
    MetricsServer(uint16_t port);
    ~MetricsServer();

    void start();
    void stop();

private:
    void run_loop();
    void handle_request(int client_fd);
    void send_response(int client_fd, const std::string& body, bool is_metrics);
    
    uint16_t port_;
    int listener_fd_;
    std::thread thread_;
    std::atomic<bool> running_;
};

} // namespace lb::metrics


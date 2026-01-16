#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace lb::logging {

struct AccessLogEntry {
    std::string client_ip;
    std::string method;
    std::string path;
    int status_code;
    size_t bytes_sent;
    size_t bytes_received;
    std::chrono::microseconds latency;
    std::string backend;
    std::chrono::system_clock::time_point timestamp;
};

class AccessLogger {
public:
    static AccessLogger& instance();

    void set_output_file(const std::string& path);
    void set_enabled(bool enabled);

    void log(const AccessLogEntry& entry);

    void start();
    void stop();

private:
    AccessLogger();
    ~AccessLogger();

    void worker_thread();
    std::string format_entry(const AccessLogEntry& entry) const;
    std::string format_timestamp(const std::chrono::system_clock::time_point& tp) const;

    bool enabled_{false};
    std::string log_file_path_;
    FILE* log_file_{nullptr};

    std::queue<AccessLogEntry> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    static constexpr size_t MAX_QUEUE_SIZE = 10000;
};

} // namespace lb::logging

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace lb::core {
class ThreadPool;
} // namespace lb::core

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

    void set_thread_pool(lb::core::ThreadPool* pool);

private:
    AccessLogger();
    ~AccessLogger();

    void worker_loop();
    void drain_queue();
    void write_entry(const AccessLogEntry& entry);
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

    std::atomic<lb::core::ThreadPool*> thread_pool_{nullptr};
    std::atomic<bool> drain_pending_{false};
};

} // namespace lb::logging

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace lb::logging {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    void set_output_file(const std::string& path);

    void log(LogLevel level, const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    void start();
    void stop();

private:
    Logger();
    ~Logger();

    void worker_thread();
    void write_log(const std::string& log_line);

    LogLevel level_;
    std::string log_file_path_;
    FILE* log_file_;

    std::queue<std::string> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    static constexpr size_t MAX_QUEUE_SIZE = 10000;
};

} // namespace lb::logging

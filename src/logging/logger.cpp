#include "logging/logger.h"
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _POSIX_C_SOURCE
#define HAVE_LOCALTIME_R
#endif
#if defined(__linux__) || defined(__APPLE__)
#define HAVE_LOCALTIME_R
#endif

namespace lb::logging {

Logger::Logger() : level_(LogLevel::INFO), log_file_(nullptr), running_(false) {}

Logger::~Logger() {
    stop();
    if (log_file_ && log_file_ != stderr)
        std::fclose(log_file_);
}

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

void Logger::set_output_file(const std::string& path) {
    log_file_path_ = path;
    if (log_file_ && log_file_ != stderr)
        std::fclose(log_file_);

    log_file_ = std::fopen(path.c_str(), "a");
    if (!log_file_)
        log_file_ = stderr;
}

void Logger::start() {
    if (running_.exchange(true))
        return;
    worker_thread_ = std::thread(&Logger::worker_thread, this);
}

void Logger::stop() {
    if (running_.exchange(false))
        queue_cv_.notify_all();
    if (worker_thread_.joinable())
        worker_thread_.join();
}

void Logger::log(LogLevel level, const std::string& message) {
    if (!running_.load(std::memory_order_acquire))
        return;
    if (level < level_)
        return;

    std::ostringstream oss;
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    struct tm time_buf;
    struct tm* time_info = nullptr;
#ifdef HAVE_LOCALTIME_R
    time_info = localtime_r(&time_t, &time_buf);
#else
    time_info = std::localtime(&time_t);
    if (time_info) {
        time_buf = *time_info;
        time_info = &time_buf;
    }
#endif
    if (!time_info)
        oss << "1970-01-01 00:00:00";
    else
        oss << std::put_time(time_info, "%Y-%m-%d %H:%M:%S");
    oss << " [";

    switch (level) {
    case LogLevel::DEBUG:
        oss << "DEBUG";
        break;
    case LogLevel::INFO:
        oss << "INFO";
        break;
    case LogLevel::WARN:
        oss << "WARN";
        break;
    case LogLevel::ERROR:
        oss << "ERROR";
        break;
    }

    oss << "] " << message << "\n";

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (log_queue_.size() >= MAX_QUEUE_SIZE)
        return;
    log_queue_.push(oss.str());
    queue_cv_.notify_one();
}

void Logger::debug(const std::string& message) {
    log(LogLevel::DEBUG, message);
}

void Logger::info(const std::string& message) {
    log(LogLevel::INFO, message);
}

void Logger::warn(const std::string& message) {
    log(LogLevel::WARN, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::ERROR, message);
}

void Logger::worker_thread() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !log_queue_.empty() || !running_; });

        while (!log_queue_.empty()) {
            std::string log_line = log_queue_.front();
            log_queue_.pop();
            lock.unlock();

            write_log(log_line);

            lock.lock();
        }
    }
}

void Logger::write_log(const std::string& log_line) {
    FILE* output = log_file_ ? log_file_ : stderr;
    std::fprintf(output, "%s", log_line.c_str());
    std::fflush(output);
}

} // namespace lb::logging

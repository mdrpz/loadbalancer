#include "logging/access_logger.h"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace lb::logging {

AccessLogger& AccessLogger::instance() {
    static AccessLogger instance;
    return instance;
}

AccessLogger::AccessLogger() = default;

AccessLogger::~AccessLogger() {
    stop();
}

void AccessLogger::set_output_file(const std::string& path) {
    log_file_path_ = path;
}

void AccessLogger::set_enabled(bool enabled) {
    enabled_ = enabled;
}

void AccessLogger::start() {
    if (running_.load() || !enabled_)
        return;

    if (!log_file_path_.empty()) {
        log_file_ = fopen(log_file_path_.c_str(), "a");
    }

    running_ = true;
    worker_thread_ = std::thread(&AccessLogger::worker_thread, this);
}

void AccessLogger::stop() {
    if (!running_.load())
        return;

    running_ = false;
    queue_cv_.notify_one();

    if (worker_thread_.joinable())
        worker_thread_.join();

    if (log_file_) {
        fclose(log_file_);
        log_file_ = nullptr;
    }
}

void AccessLogger::log(const AccessLogEntry& entry) {
    if (!enabled_ || !running_.load())
        return;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (log_queue_.size() < MAX_QUEUE_SIZE) {
        log_queue_.push(entry);
        queue_cv_.notify_one();
    }
}

void AccessLogger::worker_thread() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !log_queue_.empty() || !running_.load(); });

        while (!log_queue_.empty()) {
            AccessLogEntry entry = std::move(log_queue_.front());
            log_queue_.pop();
            lock.unlock();

            std::string log_line = format_entry(entry);

            if (log_file_) {
                fprintf(log_file_, "%s\n", log_line.c_str());
                fflush(log_file_);
            } else {
                fprintf(stdout, "%s\n", log_line.c_str());
                fflush(stdout);
            }

            lock.lock();
        }
    }
}

std::string AccessLogger::format_entry(const AccessLogEntry& entry) const {
    std::ostringstream oss;

    oss << entry.client_ip << " - - ";
    oss << "[" << format_timestamp(entry.timestamp) << "] ";
    oss << "\"" << entry.method << " " << entry.path << " HTTP/1.1\" ";
    oss << entry.status_code << " ";
    oss << entry.bytes_sent << " ";
    oss << entry.latency.count() / 1000.0 << "ms ";
    oss << "\"" << entry.backend << "\"";

    return oss.str();
}

std::string AccessLogger::format_timestamp(const std::chrono::system_clock::time_point& tp) const {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    localtime_r(&time_t_val, &tm_val);

    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%d/%b/%Y:%H:%M:%S %z");
    return oss.str();
}

} // namespace lb::logging

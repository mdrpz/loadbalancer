#include "logging/access_logger.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include "core/thread_pool.h"

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

void AccessLogger::set_thread_pool(lb::core::ThreadPool* pool) {
    auto* old = thread_pool_.exchange(pool, std::memory_order_acq_rel);
    if (pool && !old) {
        queue_cv_.notify_all();
        if (worker_thread_.joinable())
            worker_thread_.join();
    } else if (!pool && old && running_.load(std::memory_order_acquire)) {
        worker_thread_ = std::thread(&AccessLogger::worker_loop, this);
    }
}

void AccessLogger::start() {
    if (running_.load() || !enabled_)
        return;

    if (!log_file_path_.empty()) {
        log_file_ = fopen(log_file_path_.c_str(), "a");
    }

    running_ = true;
    if (!thread_pool_.load(std::memory_order_acquire))
        worker_thread_ = std::thread(&AccessLogger::worker_loop, this);
}

void AccessLogger::stop() {
    if (!running_.exchange(false))
        return;

    thread_pool_.store(nullptr, std::memory_order_release);

    queue_cv_.notify_all();
    if (worker_thread_.joinable())
        worker_thread_.join();

    while (drain_pending_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    drain_queue();

    if (log_file_) {
        fclose(log_file_);
        log_file_ = nullptr;
    }
}

void AccessLogger::log(const AccessLogEntry& entry) {
    if (!enabled_ || !running_.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (log_queue_.size() >= MAX_QUEUE_SIZE)
            return;
        log_queue_.push(entry);
    }

    auto* pool = thread_pool_.load(std::memory_order_acquire);
    if (pool) {
        bool expected = false;
        if (drain_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            pool->post([this]() { drain_queue(); });
        }
    } else {
        queue_cv_.notify_one();
    }
}

void AccessLogger::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        if (thread_pool_.load(std::memory_order_acquire))
            return;

        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
            return !log_queue_.empty() || !running_ || thread_pool_.load(std::memory_order_acquire);
        });

        if (thread_pool_.load(std::memory_order_acquire))
            return;

        while (!log_queue_.empty()) {
            AccessLogEntry entry = std::move(log_queue_.front());
            log_queue_.pop();
            lock.unlock();
            write_entry(entry);
            lock.lock();
        }
    }
}

void AccessLogger::drain_queue() {
    drain_pending_.store(false, std::memory_order_release);

    std::unique_lock<std::mutex> lock(queue_mutex_);
    while (!log_queue_.empty()) {
        AccessLogEntry entry = std::move(log_queue_.front());
        log_queue_.pop();
        lock.unlock();
        write_entry(entry);
        lock.lock();
    }
}

void AccessLogger::write_entry(const AccessLogEntry& entry) {
    std::string log_line = format_entry(entry);
    if (log_file_) {
        fprintf(log_file_, "%s\n", log_line.c_str());
        fflush(log_file_);
    } else {
        fprintf(stdout, "%s\n", log_line.c_str());
        fflush(stdout);
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

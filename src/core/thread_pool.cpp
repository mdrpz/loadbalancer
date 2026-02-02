#include "core/thread_pool.h"

namespace lb::core {

ThreadPool::ThreadPool(std::size_t workers) {
    if (workers == 0) {
        workers = 1;
    }
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    stopping_.store(true, std::memory_order_release);
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void ThreadPool::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            return;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::worker_loop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stopping_.load(std::memory_order_acquire) || !tasks_.empty();
            });
            if (stopping_.load(std::memory_order_acquire) && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            if (task) {
                task();
            }
        } catch (...) {
            // Swallow exceptions to keep workers alive.
        }
    }
}

} // namespace lb::core

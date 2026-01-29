#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lb::core {

class MemoryBudget {
public:
    MemoryBudget() = default;

    void set_limit_bytes(uint64_t bytes) {
        limit_bytes_.store(bytes, std::memory_order_release);
    }

    void set_limit_mb(uint32_t mb) {
        set_limit_bytes(static_cast<uint64_t>(mb) * 1024ULL * 1024ULL);
    }

    [[nodiscard]] uint64_t limit_bytes() const {
        return limit_bytes_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t used_bytes() const {
        return used_bytes_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_exceeded() const {
        uint64_t lim = limit_bytes();
        return lim > 0 && used_bytes() >= lim;
    }

    bool try_reserve(uint64_t n) {
        uint64_t lim = limit_bytes();
        if (lim == 0 || n == 0)
            return true;

        uint64_t cur = used_bytes_.load(std::memory_order_relaxed);
        while (true) {
            if (cur + n > lim)
                return false;
            if (used_bytes_.compare_exchange_weak(cur, cur + n, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void release(uint64_t n) {
        if (n == 0)
            return;
        uint64_t cur = used_bytes_.load(std::memory_order_relaxed);
        while (true) {
            uint64_t next = (cur > n) ? (cur - n) : 0;
            if (used_bytes_.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                return;
            }
        }
    }

private:
    std::atomic<uint64_t> used_bytes_{0};
    std::atomic<uint64_t> limit_bytes_{0};
};

} // namespace lb::core

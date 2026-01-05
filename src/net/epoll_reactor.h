#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace lb::net {

class Connection;

enum class EventType { READ, WRITE, ERROR, HUP };

using EventCallback = std::function<void(int fd, EventType type)>;

class EpollReactor {
public:
    EpollReactor();
    ~EpollReactor();

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    bool add_fd(int fd, uint32_t events, EventCallback callback);
    bool mod_fd(int fd, uint32_t events) const;
    bool del_fd(int fd);

    void run();
    void stop();

    void set_periodic_callback(std::function<void()> callback, uint32_t interval_ms);

private:
    int epoll_fd_;
    bool running_;
    static constexpr int MAX_EVENTS = 64;
    std::unordered_map<int, EventCallback> callbacks_;

    std::function<void()> periodic_callback_;
    uint32_t periodic_interval_ms_;
    std::chrono::steady_clock::time_point last_periodic_check_;
};

} // namespace lb::net

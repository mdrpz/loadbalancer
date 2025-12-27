#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace lb::net {

class Connection;

enum class EventType {
    READ,
    WRITE,
    ERROR,
    HUP
};

using EventCallback = std::function<void(int fd, EventType type)>;

class EpollReactor {
public:
    EpollReactor();
    ~EpollReactor();

    // Non-copyable
    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    bool add_fd(int fd, uint32_t events, EventCallback callback);
    bool mod_fd(int fd, uint32_t events);
    bool del_fd(int fd);

    // Run event loop (blocks until stop() is called)
    void run();
    void stop();

private:
    int epoll_fd_;
    bool running_;
    static constexpr int MAX_EVENTS = 64;
    std::unordered_map<int, EventCallback> callbacks_;
};

} // namespace lb::net


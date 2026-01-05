#include "net/epoll_reactor.h"
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <chrono>
#include <stdexcept>
#include <unordered_map>

namespace lb::net {

EpollReactor::EpollReactor()
    : running_(false), periodic_interval_ms_(0),
      last_periodic_check_(std::chrono::steady_clock::now()) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error("Failed to create epoll instance");
}

EpollReactor::~EpollReactor() {
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

bool EpollReactor::add_fd(int fd, uint32_t events, EventCallback callback) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        return false;
    callbacks_[fd] = std::move(callback);
    return true;
}

bool EpollReactor::mod_fd(int fd, uint32_t events) const {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EpollReactor::del_fd(int fd) {
    epoll_event ev{};
    bool result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) == 0;
    if (result || (errno != EBADF && errno != ENOENT))
        callbacks_.erase(fd);
    return result;
}

void EpollReactor::set_periodic_callback(std::function<void()> callback, uint32_t interval_ms) {
    periodic_callback_ = std::move(callback);
    periodic_interval_ms_ = interval_ms;
    last_periodic_check_ = std::chrono::steady_clock::now();
}

void EpollReactor::run() {
    running_ = true;
    epoll_event events[MAX_EVENTS];

    int timeout_ms = periodic_interval_ms_ > 0 ? static_cast<int>(periodic_interval_ms_) : 1000;

    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        if (periodic_callback_ && periodic_interval_ms_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_periodic_check_)
                    .count();

            if (elapsed >= static_cast<int64_t>(periodic_interval_ms_)) {
                periodic_callback_();
                last_periodic_check_ = now;
            }
        }

        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            auto it = callbacks_.find(fd);
            if (it == callbacks_.end())
                continue;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                it->second(fd, EventType::ERROR);
                it = callbacks_.find(fd);
                if (it == callbacks_.end())
                    continue;
            }
            if (ev & EPOLLIN) {
                it->second(fd, EventType::READ);
                it = callbacks_.find(fd); // Re-check
                if (it == callbacks_.end())
                    continue;
            }
            if (ev & EPOLLOUT) {
                it->second(fd, EventType::WRITE);
            }
        }
    }
}

void EpollReactor::stop() {
    running_ = false;
}

} // namespace lb::net

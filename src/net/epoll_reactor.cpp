#include "net/epoll_reactor.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <stdexcept>

namespace lb::net {

EpollReactor::EpollReactor() : running_(false) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll instance");
    }
}

EpollReactor::~EpollReactor() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool EpollReactor::add_fd(int fd, uint32_t events, EventCallback callback) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return false;
    }
    
    callbacks_[fd] = std::move(callback);
    return true;
}

bool EpollReactor::mod_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EpollReactor::del_fd(int fd) {
    epoll_event ev{}; // epoll_ctl with EPOLL_CTL_DEL ignores ev
    bool result = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) == 0;
    callbacks_.erase(fd);
    return result;
}

void EpollReactor::run() {
    running_ = true;
    epoll_event events[MAX_EVENTS];

    while (running_) {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            auto it = callbacks_.find(fd);
            if (it == callbacks_.end()) {
                continue; // Callback removed or never registered
            }

            // Handle multiple events - invoke callback for each
            if (ev & (EPOLLERR | EPOLLHUP)) {
                it->second(fd, EventType::ERROR);
            }
            if (ev & EPOLLIN) {
                it->second(fd, EventType::READ);
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


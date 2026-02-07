#include "core/splice_forwarder.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

#include <sys/epoll.h>
#include <algorithm>
#include <utility>
#include "logging/logger.h"

namespace lb::core {

SpliceForwarder::SpliceForwarder(net::EpollReactor& reactor,
                                 std::function<void(int)> start_backpressure,
                                 std::function<void(int)> clear_backpressure,
                                 std::function<void(int)> close_connection)
    : reactor_(reactor), start_backpressure_(std::move(start_backpressure)),
      clear_backpressure_(std::move(clear_backpressure)),
      close_connection_(std::move(close_connection)) {}

SpliceForwarder::~SpliceForwarder() {
    for (auto& [key, pipe] : pipes_) {
        if (pipe.read_fd >= 0) {
            ::close(pipe.read_fd);
        }
        if (pipe.write_fd >= 0) {
            ::close(pipe.write_fd);
        }
    }
    pipes_.clear();
}

bool SpliceForwarder::is_available() {
#ifdef __linux__
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return true;
#else
    return false;
#endif
}

void SpliceForwarder::forward(net::Connection* from, net::Connection* to) {
    if (!from || !to) {
        return;
    }

    if (from->state() != net::ConnectionState::ESTABLISHED ||
        to->state() != net::ConnectionState::ESTABLISHED) {
        return;
    }

#ifdef __linux__
    PipePair* pipe = get_or_create_pipe(from->fd(), to->fd());
    if (!pipe) {
        forward_with_buffers(from, to);
        return;
    }

    if (pipe->pending > 0) {
        ssize_t out = splice_out(pipe, to->fd());
        if (out < 0) {
            close_connection_(to->fd());
            return;
        }
        if (out == 0 && pipe->pending > 0) {
            start_backpressure_(from->fd());
            reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
            return;
        }
        pipe->pending -= out;
    }

    ssize_t in = splice_in(from->fd(), pipe);
    lb::logging::Logger::instance().debug("splice_in: fd=" + std::to_string(from->fd()) +
                                          " -> pipe, result=" + std::to_string(in));
    if (in == -1) {
        close_connection_(from->fd());
        return;
    }
    if (in == -2) {
        return;
    }
    if (in == 0) {
        lb::logging::Logger::instance().debug("splice_in: EOF on fd=" + std::to_string(from->fd()));
        close_connection_(from->fd());
        return;
    }

    pipe->pending += in;

    ssize_t out = splice_out(pipe, to->fd());
    lb::logging::Logger::instance().debug("splice_out: pipe -> fd=" + std::to_string(to->fd()) +
                                          ", result=" + std::to_string(out));
    if (out < 0) {
        close_connection_(to->fd());
        return;
    }
    pipe->pending -= out;

    if (pipe->pending > 0) {
        start_backpressure_(from->fd());
        reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
    } else {
        clear_backpressure_(from->fd());
    }
#else
    forward_with_buffers(from, to);
#endif
}

void SpliceForwarder::cleanup(int fd) {
    std::vector<uint64_t> to_remove;
    for (const auto& [key, pipe] : pipes_) {
        int src_fd = static_cast<int>(key >> 32);
        int dst_fd = static_cast<int>(key & 0xFFFFFFFF);
        if (src_fd == fd || dst_fd == fd) {
            to_remove.push_back(key);
        }
    }

    for (uint64_t key : to_remove) {
        auto it = pipes_.find(key);
        if (it != pipes_.end()) {
            if (it->second.read_fd >= 0) {
                ::close(it->second.read_fd);
            }
            if (it->second.write_fd >= 0) {
                ::close(it->second.write_fd);
            }
            pipes_.erase(it);
        }
    }
}

SpliceForwarder::PipePair* SpliceForwarder::get_or_create_pipe(int src_fd, int dst_fd) {
#ifdef __linux__
    uint64_t key = (static_cast<uint64_t>(src_fd) << 32) | static_cast<uint64_t>(dst_fd);

    auto it = pipes_.find(key);
    if (it != pipes_.end()) {
        return &it->second;
    }

    int pipefd[2];
    if (pipe2(pipefd, O_NONBLOCK) != 0) {
        lb::logging::Logger::instance().warn("Failed to create pipe for splice: " +
                                             std::string(strerror(errno)));
        return nullptr;
    }

    fcntl(pipefd[0], F_SETPIPE_SZ, MAX_PIPE_SIZE);

    PipePair pipe;
    pipe.read_fd = pipefd[0];
    pipe.write_fd = pipefd[1];
    pipe.pending = 0;

    auto [inserted_it, success] = pipes_.emplace(key, pipe);
    if (!success) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return nullptr;
    }

    return &inserted_it->second;
#else
    (void)src_fd;
    (void)dst_fd;
    return nullptr;
#endif
}

ssize_t SpliceForwarder::splice_in(int src_fd, PipePair* pipe) {
#ifdef __linux__
    if (!pipe || pipe->write_fd < 0) {
        return -1;
    }

    if (pipe->pending >= MAX_PIPE_SIZE) {
        return -2;
    }

    size_t max_splice = std::min(SPLICE_CHUNK_SIZE, MAX_PIPE_SIZE - pipe->pending);

    ssize_t n = splice(src_fd, nullptr, pipe->write_fd, nullptr, max_splice,
                       SPLICE_F_NONBLOCK | SPLICE_F_MOVE);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2;
        }
        lb::logging::Logger::instance().error("splice_in failed: src_fd=" + std::to_string(src_fd) +
                                              " pipe_write=" + std::to_string(pipe->write_fd) +
                                              " errno=" + std::to_string(errno) + " (" +
                                              std::string(strerror(errno)) + ")");
        return -1;
    }

    return n;
#else
    (void)src_fd;
    (void)pipe;
    return -1;
#endif
}

ssize_t SpliceForwarder::splice_out(PipePair* pipe, int dst_fd) {
#ifdef __linux__
    if (!pipe || pipe->read_fd < 0 || pipe->pending == 0) {
        return 0;
    }

    size_t to_splice = std::min(pipe->pending, SPLICE_CHUNK_SIZE);

    ssize_t n = splice(pipe->read_fd, nullptr, dst_fd, nullptr, to_splice,
                       SPLICE_F_NONBLOCK | SPLICE_F_MOVE);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        lb::logging::Logger::instance().error(
            "splice_out failed: pipe_read=" + std::to_string(pipe->read_fd) +
            " dst_fd=" + std::to_string(dst_fd) + " errno=" + std::to_string(errno) + " (" +
            std::string(strerror(errno)) + ")");
        return -1;
    }

    return n;
#else
    (void)pipe;
    (void)dst_fd;
    return -1;
#endif
}

void SpliceForwarder::forward_with_buffers(net::Connection* from, net::Connection* to) {
    auto& read_buf = from->read_buffer();
    auto& write_buf = to->write_buffer();

    if (read_buf.empty()) {
        return;
    }

    size_t available = to->write_available();
    size_t to_copy = std::min(read_buf.size(), available);
    if (to_copy == 0) {
        start_backpressure_(from->fd());
        reactor_.mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    if (!write_buf.empty()) {
        start_backpressure_(from->fd());
    } else {
        clear_backpressure_(from->fd());
    }

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
    reactor_.mod_fd(from->fd(), EPOLLIN | EPOLLOUT);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }

    if (!to->write_buffer().empty()) {
        start_backpressure_(from->fd());
    } else {
        clear_backpressure_(from->fd());
    }
}

} // namespace lb::core

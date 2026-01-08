#pragma once

#include <functional>
#include <unordered_map>
#include "net/connection.h"
#include "net/epoll_reactor.h"

namespace lb::core {

class SpliceForwarder {
public:
    SpliceForwarder(net::EpollReactor& reactor, std::function<void(int)> start_backpressure,
                    std::function<void(int)> clear_backpressure,
                    std::function<void(int)> close_connection);

    ~SpliceForwarder();

    void forward(net::Connection* from, net::Connection* to);

    void cleanup(int fd);

    static bool is_available();

private:
    struct PipePair {
        int read_fd = -1;
        int write_fd = -1;
        size_t pending = 0;
    };

    PipePair* get_or_create_pipe(int src_fd, int dst_fd);

    void forward_with_buffers(net::Connection* from, net::Connection* to);

    static ssize_t splice_in(int src_fd, PipePair* pipe);

    static ssize_t splice_out(PipePair* pipe, int dst_fd);

    net::EpollReactor& reactor_;
    std::function<void(int)> start_backpressure_;
    std::function<void(int)> clear_backpressure_;
    std::function<void(int)> close_connection_;

    std::unordered_map<uint64_t, PipePair> pipes_;

    static constexpr size_t SPLICE_CHUNK_SIZE = 65536;
    static constexpr size_t MAX_PIPE_SIZE = 1048576;
};

} // namespace lb::core

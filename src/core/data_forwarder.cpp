#include "core/data_forwarder.h"
#include <sys/epoll.h>

#include <utility>

namespace lb::core {

DataForwarder::DataForwarder(net::EpollReactor& reactor,
                             std::function<void(int)> start_backpressure,
                             std::function<void(int)> clear_backpressure,
                             std::function<void(int)> close_connection)
    : reactor_(reactor), start_backpressure_(std::move(std::move(start_backpressure))),
      clear_backpressure_(std::move(std::move(clear_backpressure))),
      close_connection_(std::move(std::move(close_connection))) {}

void DataForwarder::forward(net::Connection* from, net::Connection* to) {
    if (!from || !to)
        return;

    if (from->state() != net::ConnectionState::ESTABLISHED ||
        to->state() != net::ConnectionState::ESTABLISHED)
        return;

    auto& read_buf = from->read_buffer();
    auto& write_buf = to->write_buffer();

    if (read_buf.empty())
        return;

    size_t available = to->write_available();
    size_t to_copy = std::min(read_buf.size(), available);
    if (to_copy == 0) {
        start_backpressure_(from->fd());
        reactor_.mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    bool was_buffer_full = from->buffer_full();
    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    if (was_buffer_full && from->has_read_space()) {
        from->set_buffer_full(false);
    }

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);

    uint32_t from_events = EPOLLOUT;
    if (!from->memory_blocked() && !from->buffer_full()) {
        from_events |= EPOLLIN;
    }
    reactor_.mod_fd(from->fd(), from_events);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }

    auto has_pending_send = [](net::Connection* conn) {
        return conn && (!conn->write_buffer().empty() || conn->pending_kernel_bytes() > 0);
    };

    if (has_pending_send(to)) {
        start_backpressure_(from->fd());
    } else {
        clear_backpressure_(from->fd());
    }
}

} // namespace lb::core

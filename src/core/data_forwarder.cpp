#include "core/data_forwarder.h"
#include <sys/epoll.h>

#include <utility>

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
    if (available == 0) {
        start_backpressure_(from->fd());
        reactor_.mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    clear_backpressure_(from->fd());

    size_t to_copy = std::min(read_buf.size(), available);
    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
    reactor_.mod_fd(from->fd(), EPOLLIN | EPOLLOUT);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }
}

} // namespace lb::core

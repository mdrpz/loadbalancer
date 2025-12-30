#include "core/data_forwarder.h"
#include <sys/epoll.h>

namespace lb::core {

DataForwarder::DataForwarder(net::EpollReactor& reactor,
                             std::function<void(int)> start_backpressure,
                             std::function<void(int)> clear_backpressure,
                             std::function<void(int)> close_connection)
    : reactor_(reactor),
      start_backpressure_(start_backpressure),
      clear_backpressure_(clear_backpressure),
      close_connection_(close_connection) {
}

void DataForwarder::forward(net::Connection* from, net::Connection* to) {
    if (!from || !to) {
        return;
    }

    // Validate both connections are established
    if (from->state() != net::ConnectionState::ESTABLISHED ||
        to->state() != net::ConnectionState::ESTABLISHED) {
        return;
    }

    // Copy data from from->read_buf to to->write_buf
    auto& read_buf = from->read_buffer();
    auto& write_buf = to->write_buffer();

    if (read_buf.empty()) {
        return;
    }

    // Check if destination buffer has space
    size_t available = to->write_available();
    if (available == 0) {
        // Destination buffer full - start backpressure tracking
        start_backpressure_(from->fd());
        
        // Stop reading from source
        // Only disable reads, keep writes enabled
        reactor_.mod_fd(from->fd(), EPOLLOUT);
        return;
    }

    // Buffer has space - clear backpressure if it was active
    clear_backpressure_(from->fd());

    // Copy data
    size_t to_copy = std::min(read_buf.size(), available);
    write_buf.insert(write_buf.end(), read_buf.begin(), read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    // Enable write events on destination
    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);

    // Re-enable reads on source if we had disabled them due to backpressure
    // and now there's space in destination
    reactor_.mod_fd(from->fd(), EPOLLIN | EPOLLOUT);

    // Try to write immediately
    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }
}

} // namespace lb::core


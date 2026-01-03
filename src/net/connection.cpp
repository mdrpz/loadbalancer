#include "net/connection.h"
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>

namespace lb::net {

Connection::Connection(int fd) : fd_(fd), state_(ConnectionState::ESTABLISHED), peer_(nullptr) {
    read_buf_.reserve(MAX_BUFFER_SIZE);
    write_buf_.reserve(MAX_BUFFER_SIZE);
}

Connection::~Connection() {
    close();
}

size_t Connection::read_available() const {
    return read_buf_.size();
}

size_t Connection::write_available() const {
    return MAX_BUFFER_SIZE - write_buf_.size();
}

bool Connection::read_from_fd() {
    if (read_buf_.size() >= MAX_BUFFER_SIZE)
        return false;

    size_t to_read = MAX_BUFFER_SIZE - read_buf_.size();
    size_t old_size = read_buf_.size();
    read_buf_.resize(read_buf_.size() + to_read);

    ssize_t n = ::read(fd_, read_buf_.data() + old_size, to_read);
    if (n < 0) {
        read_buf_.resize(old_size);
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (n == 0) {
        read_buf_.resize(old_size);
        return false;
    }
    read_buf_.resize(old_size + n);
    return true;
}
bool Connection::write_to_fd() {
    if (write_buf_.empty())
        return true;

    ssize_t n = ::write(fd_, write_buf_.data(), write_buf_.size());
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    write_buf_.erase(write_buf_.begin(), write_buf_.begin() + n);
    return true;
}
void Connection::close() {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    state_ = ConnectionState::CLOSED;
    if (peer_)
        peer_->peer_ = nullptr;
}

} // namespace lb::net

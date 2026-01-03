#include "net/tcp_listener.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace lb::net {

TcpListener::TcpListener() : fd_(-1) {}

TcpListener::~TcpListener() {
    if (fd_ >= 0)
        ::close(fd_);
}

TcpListener::TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool TcpListener::bind(const std::string& host, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd_ < 0)
        return false;

    int opt = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

bool TcpListener::listen(int backlog) const {
    if (fd_ < 0)
        return false;
    return ::listen(fd_, backlog) == 0;
}

int TcpListener::accept() const {
    if (fd_ < 0)
        return -1;

    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len,
                              SOCK_NONBLOCK | SOCK_CLOEXEC);
    return client_fd;
}

} // namespace lb::net

#pragma once

#include <cstdint>
#include <string>

namespace lb::net {

class TcpListener {
public:
    TcpListener();
    ~TcpListener();

    // Non-copyable
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Movable
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;

    bool bind(const std::string& host, uint16_t port);
    bool listen(int backlog = 128) const;
    int accept() const; // Returns fd or -1 on error

    [[nodiscard]] int fd() const {
        return fd_;
    }
    [[nodiscard]] bool is_bound() const {
        return fd_ >= 0;
    }

private:
    int fd_;
};

} // namespace lb::net

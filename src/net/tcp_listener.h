#pragma once

#include <cstdint>
#include <string>

namespace lb::net {

class TcpListener {
public:
    TcpListener();
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) noexcept;

    bool bind(const std::string& host, uint16_t port);
    [[nodiscard]] bool listen(int backlog = 128) const;
    [[nodiscard]] int accept() const;

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

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct ssl_st;
using SSL = struct ssl_st;

namespace lb::net {

enum class ConnectionState { HANDSHAKE, CONNECTING, ESTABLISHED, CLOSED };

class Connection {
public:
    Connection(int fd);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] int fd() const {
        return fd_;
    }
    [[nodiscard]] ConnectionState state() const {
        return state_;
    }
    void set_state(ConnectionState state) {
        state_ = state;
    }

    [[nodiscard]] Connection* peer() const {
        return peer_;
    }
    void set_peer(Connection* peer) {
        peer_ = peer;
    }

    [[nodiscard]] size_t read_available() const;
    [[nodiscard]] size_t write_available() const;
    bool read_from_fd();
    bool write_to_fd();

    std::vector<uint8_t>& read_buffer() {
        return read_buf_;
    }
    std::vector<uint8_t>& write_buffer() {
        return write_buf_;
    }

    void clear_buffers() {
        read_buf_.clear();
        write_buf_.clear();
    }

    void close();

    void set_ssl(SSL* ssl) {
        ssl_ = ssl;
    }
    [[nodiscard]] SSL* ssl() const {
        return ssl_;
    }
    [[nodiscard]] bool is_tls() const {
        return ssl_ != nullptr;
    }

    [[nodiscard]] size_t bytes_read() const {
        return bytes_read_;
    }
    [[nodiscard]] size_t bytes_written() const {
        return bytes_written_;
    }

private:
    int fd_;
    ConnectionState state_;
    Connection* peer_;
    SSL* ssl_;

    static constexpr size_t MAX_BUFFER_SIZE = 64 * 1024;
    std::vector<uint8_t> read_buf_;
    std::vector<uint8_t> write_buf_;

    size_t bytes_read_{0};
    size_t bytes_written_{0};
};

} // namespace lb::net

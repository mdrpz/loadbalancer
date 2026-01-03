#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace lb::net {

enum class ConnectionState { HANDSHAKE, CONNECTING, ESTABLISHED, CLOSED };

class Connection {
public:
    Connection(int fd);
    ~Connection();

    // Non-copyable
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

    // Buffer operations
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

    void close();

private:
    int fd_;
    ConnectionState state_;
    Connection* peer_; // Cross-wired connection (client <-> backend)

    static constexpr size_t MAX_BUFFER_SIZE = 64 * 1024; // 64KB
    std::vector<uint8_t> read_buf_;
    std::vector<uint8_t> write_buf_;
};

} // namespace lb::net

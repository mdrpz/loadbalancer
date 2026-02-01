#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ssl_st;
using SSL = struct ssl_st;

namespace lb::net {

enum class ConnectionState { HANDSHAKE, CONNECTING, ESTABLISHED, CLOSED };

class Connection {
public:
    using ReserveBytesFn = std::function<bool(size_t)>;
    using ReleaseBytesFn = std::function<void(size_t)>;

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
    [[nodiscard]] bool has_read_space() const;
    bool read_from_fd();
    bool write_to_fd();

    std::vector<uint8_t>& read_buffer() {
        return read_buf_;
    }
    std::vector<uint8_t>& write_buffer() {
        return write_buf_;
    }

    void clear_buffers() {
        if (release_bytes_) {
            release_bytes_(read_buf_.size() + write_buf_.size());
        }
        read_buf_.clear();
        write_buf_.clear();
        memory_blocked_ = false;
        buffer_full_ = false;
    }

    void set_memory_accounting(ReserveBytesFn reserve_bytes, ReleaseBytesFn release_bytes) {
        reserve_bytes_ = std::move(reserve_bytes);
        release_bytes_ = std::move(release_bytes);
    }

    [[nodiscard]] bool memory_blocked() const {
        return memory_blocked_;
    }

    [[nodiscard]] bool buffer_full() const {
        return buffer_full_;
    }
    void set_buffer_full(bool full) {
        buffer_full_ = full;
    }

    bool try_reserve_additional_bytes(size_t n) {
        if (n == 0) {
            memory_blocked_ = false;
            return true;
        }
        if (!reserve_bytes_) {
            memory_blocked_ = false;
            return true;
        }
        if (!reserve_bytes_(n)) {
            memory_blocked_ = true;
            return false;
        }
        memory_blocked_ = false;
        return true;
    }

    void release_accounted_bytes(size_t n) {
        if (n == 0)
            return;
        if (release_bytes_)
            release_bytes_(n);
        memory_blocked_ = false;
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

    ReserveBytesFn reserve_bytes_;
    ReleaseBytesFn release_bytes_;
    bool memory_blocked_{false};
    bool buffer_full_{false};
};

} // namespace lb::net

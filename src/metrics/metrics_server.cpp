#include "metrics/metrics_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include "core/thread_pool.h"
#include "metrics/metrics.h"

namespace lb::metrics {

MetricsServer::MetricsServer(uint16_t port) : port_(port), listener_fd_(-1), running_(false) {}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::set_thread_pool(lb::core::ThreadPool* pool) {
    thread_pool_ = pool;
}

void MetricsServer::start() {
    if (running_.load())
        return;

    listener_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd_ < 0)
        return;

    int opt = 1;
    if (setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }

    int flags = fcntl(listener_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(listener_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listener_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }

    if (listen(listener_fd_, 128) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }

    running_ = true;
    if (thread_pool_) {
        pool_task_active_.store(true, std::memory_order_release);
        thread_pool_->post([this]() { run_loop(); });
    } else {
        thread_ = std::thread(&MetricsServer::run_loop, this);
    }
}

void MetricsServer::stop() {
    if (!running_.exchange(false))
        return;

    if (listener_fd_ >= 0) {
        int fd = listener_fd_;
        listener_fd_ = -1;
        ::close(fd);
    }

    if (thread_.joinable())
        thread_.join();

    while (pool_task_active_.load(std::memory_order_acquire)) {
        usleep(1000);
    }
}

void MetricsServer::run_loop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            continue;
        }

        if (thread_pool_) {
            thread_pool_->post([client_fd]() {
                handle_request(client_fd);
                ::close(client_fd);
            });
        } else {
            handle_request(client_fd);
            ::close(client_fd);
        }
    }
    pool_task_active_.store(false, std::memory_order_release);
}

void MetricsServer::handle_request(int client_fd) {
    char buffer[4096];
    ssize_t total_read = 0;
    ssize_t bytes_read = 0;
    const auto buffer_size = static_cast<ssize_t>(sizeof(buffer) - 1);

    while (total_read < buffer_size) {
        bytes_read =
            recv(client_fd, buffer + total_read, static_cast<size_t>(buffer_size - total_read), 0);
        if (bytes_read <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return;
        }
        total_read += bytes_read;

        buffer[total_read] = '\0';
        if (strstr(buffer, "\r\n\r\n") || strstr(buffer, "\n\n"))
            break;
    }

    buffer[total_read] = '\0';

    std::string request(buffer, total_read);
    if (request.find("GET /metrics") == std::string::npos &&
        request.find("GET /") == std::string::npos) {
        send_response(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", false);
        return;
    }

    std::string metrics = Metrics::instance().export_prometheus();
    send_response(client_fd, metrics, true);
}

void MetricsServer::send_response(int client_fd, const std::string& body, bool is_metrics) {
    std::ostringstream response;

    if (is_metrics) {
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: text/plain; version=0.0.4\r\n";
        response << "Content-Length: " << body.size() << "\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        response << body;
    } else {
        response << body;
    }

    std::string response_str = response.str();
    ssize_t sent = 0;
    ssize_t total = response_str.size();

    while (sent < total) {
        ssize_t bytes = send(client_fd, response_str.c_str() + sent, total - sent, 0);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        sent += bytes;
    }
}

} // namespace lb::metrics

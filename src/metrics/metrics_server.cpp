#include "metrics/metrics_server.h"
#include "metrics/metrics.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sstream>

namespace lb::metrics {

MetricsServer::MetricsServer(uint16_t port)
    : port_(port), listener_fd_(-1), running_(false) {
}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start() {
    if (running_.load()) {
        return; // Already running
    }
    
    // Create socket
    listener_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd_ < 0) {
        return; // Failed to create socket
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }
    
    // Make non-blocking
    int flags = fcntl(listener_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(listener_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }
    
    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    
    if (bind(listener_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }
    
    // Listen
    if (listen(listener_fd_, 128) < 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
        return;
    }
    
    running_ = true;
    thread_ = std::thread(&MetricsServer::run_loop, this);
}

void MetricsServer::stop() {
    running_ = false;
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MetricsServer::run_loop() {
    while (running_.load()) {
        // Accept connection
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connection available, sleep briefly
                usleep(10000); // 10ms
                continue;
            }
            // Error accepting, but continue
            continue;
        }
        
        // Handle request
        handle_request(client_fd);
        ::close(client_fd);
    }
}

void MetricsServer::handle_request(int client_fd) {
    // Simple HTTP request parser - read until we get a complete request line
    char buffer[4096];
    ssize_t total_read = 0;
    ssize_t bytes_read = 0;
    const ssize_t buffer_size = static_cast<ssize_t>(sizeof(buffer) - 1);
    
    // Read request (with timeout protection)
    while (total_read < buffer_size) {
        bytes_read = recv(client_fd, buffer + total_read, 
                          static_cast<size_t>(buffer_size - total_read), 0);
        if (bytes_read <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Would block, but we've read enough
                break;
            }
            return; // Error or closed
        }
        total_read += bytes_read;
        
        // Check if we have a complete request line
        buffer[total_read] = '\0';
        if (strstr(buffer, "\r\n\r\n") || strstr(buffer, "\n\n")) {
            break; // Complete request
        }
    }
    
    buffer[total_read] = '\0';
    
    // Simple check for GET /metrics
    std::string request(buffer, total_read);
    if (request.find("GET /metrics") == std::string::npos && 
        request.find("GET /") == std::string::npos) {
        // Not a GET request or wrong path - send 404
        send_response(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", false);
        return;
    }
    
    // Get metrics
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Would block, but we'll try to send what we can
                continue;
            }
            break; // Error
        }
        sent += bytes;
    }
}

} // namespace lb::metrics

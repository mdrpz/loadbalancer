#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <chrono>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "core/backend_node.h"
#include "health/health_checker.h"

class TestServer {
public:
    TestServer(uint16_t port, bool should_accept = true, bool http_healthy = false)
        : port_(port), should_accept_(should_accept), http_healthy_(http_healthy), running_(false) {
    }

    void start() {
        running_ = true;
        server_thread_ = std::thread(&TestServer::run, this);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void stop() {
        running_ = false;
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    void set_should_accept(bool accept) {
        should_accept_ = accept;
    }

    void set_http_healthy(bool healthy) {
        http_healthy_ = healthy;
    }

private:
    void run() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            return;
        }

        if (listen(sock, 5) < 0) {
            ::close(sock);
            return;
        }

        while (running_) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sock, &read_fds);

            struct timeval timeout {};
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;

            int result = select(sock + 1, &read_fds, nullptr, nullptr, &timeout);
            if (result > 0 && FD_ISSET(sock, &read_fds)) {
                int client = accept(sock, nullptr, nullptr);
                if (client >= 0) {
                    if (should_accept_) {
                        handle_client(client);
                    } else {
                        ::close(client);
                    }
                }
            }
        }

        ::close(sock);
    }

    void handle_client(int client) {
        char buffer[4096];
        ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) {
            buffer[received] = '\0';
            std::string request(buffer, received);

            if (request.find("GET /") != std::string::npos || request.find("GET /health") != std::string::npos) {
                if (http_healthy_) {
                    const char* response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Length: 0\r\n"
                                           "\r\n";
                    send(client, response, strlen(response), 0);
                } else {
                    const char* response = "HTTP/1.1 503 Service Unavailable\r\n"
                                           "Content-Length: 0\r\n"
                                           "\r\n";
                    send(client, response, strlen(response), 0);
                }
            } else {
                const char* response = "HTTP/1.1 404 Not Found\r\n"
                                       "Content-Length: 0\r\n"
                                       "\r\n";
                send(client, response, strlen(response), 0);
            }
        }
        ::close(client);
    }

    uint16_t port_;
    bool should_accept_;
    bool http_healthy_;
    std::atomic<bool> running_;
    std::thread server_thread_;
};

void test_health_tcp_healthy() {
    std::cout << "Testing TCP health check - healthy backend...\n";

    TestServer server(18000, true);
    server.start();

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18000);
    backend->set_state(lb::core::BackendState::HEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "tcp", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(backend->state() == lb::core::BackendState::HEALTHY);

    checker.stop();
    server.stop();

    std::cout << "TCP healthy backend test passed!\n";
}

void test_health_tcp_unhealthy_transition() {
    std::cout << "Testing TCP health check - healthy to unhealthy transition...\n";

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18001);
    backend->set_state(lb::core::BackendState::HEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "tcp", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    assert(backend->state() == lb::core::BackendState::UNHEALTHY);

    checker.stop();

    std::cout << "TCP unhealthy transition test passed!\n";
}

void test_health_tcp_unhealthy_to_healthy() {
    std::cout << "Testing TCP health check - unhealthy to healthy transition...\n";

    TestServer server(18002, false);
    server.start();

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18002);
    backend->set_state(lb::core::BackendState::UNHEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "tcp", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server.set_should_accept(true);

    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    assert(backend->state() == lb::core::BackendState::HEALTHY);

    checker.stop();
    server.stop();

    std::cout << "TCP unhealthy to healthy transition test passed!\n";
}

void test_health_http_healthy() {
    std::cout << "Testing HTTP health check - healthy backend...\n";

    TestServer server(18003, true, true);
    server.start();

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18003);
    backend->set_state(lb::core::BackendState::HEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "http", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(backend->state() == lb::core::BackendState::HEALTHY);

    checker.stop();
    server.stop();

    std::cout << "HTTP healthy backend test passed!\n";
}

void test_health_http_unhealthy_transition() {
    std::cout << "Testing HTTP health check - healthy to unhealthy transition...\n";

    TestServer server(18004, true, false);
    server.start();

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18004);
    backend->set_state(lb::core::BackendState::HEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "http", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    assert(backend->state() == lb::core::BackendState::UNHEALTHY);

    checker.stop();
    server.stop();

    std::cout << "HTTP unhealthy transition test passed!\n";
}

void test_health_flapping() {
    std::cout << "Testing health check flapping behavior...\n";

    TestServer server(18005, true, false);
    server.start();

    auto backend = std::make_shared<lb::core::BackendNode>("127.0.0.1", 18005);
    backend->set_state(lb::core::BackendState::HEALTHY);

    lb::health::HealthChecker checker;
    checker.configure(100, 500, 3, 2, "http", "/health");
    checker.add_backend(backend);
    checker.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    assert(backend->state() == lb::core::BackendState::UNHEALTHY);

    server.set_http_healthy(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(backend->state() == lb::core::BackendState::HEALTHY);

    server.set_http_healthy(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    assert(backend->state() == lb::core::BackendState::UNHEALTHY);

    checker.stop();
    server.stop();

    std::cout << "Flapping behavior test passed!\n";
}

int main() {
    try {
        test_health_tcp_healthy();
        test_health_tcp_unhealthy_transition();
        test_health_tcp_unhealthy_to_healthy();
        test_health_http_healthy();
        test_health_http_unhealthy_transition();
        test_health_flapping();
        std::cout << "\nAll health checker tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
}

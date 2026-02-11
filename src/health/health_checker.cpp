#include "health/health_checker.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <string>
#include <thread>
#include "logging/logger.h"

namespace lb::health {

HealthChecker::HealthChecker()
    : running_(false), interval_ms_(5000), timeout_ms_(500), failure_threshold_(3),
      success_threshold_(2), health_check_type_("tcp") {}

HealthChecker::~HealthChecker() {
    stop();
}

void HealthChecker::add_backend(const std::shared_ptr<lb::core::BackendNode>& backend) {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    backends_.push_back(backend);

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    consecutive_failures_[backend] = 0;
    consecutive_successes_[backend] = 0;
}

void HealthChecker::remove_backend(const std::shared_ptr<lb::core::BackendNode>& backend) {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    backends_.erase(std::remove(backends_.begin(), backends_.end(), backend), backends_.end());

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    consecutive_failures_.erase(backend);
    consecutive_successes_.erase(backend);
}

void HealthChecker::configure(uint32_t interval_ms, uint32_t timeout_ms, uint32_t failure_threshold,
                              uint32_t success_threshold, const std::string& type,
                              const std::string& path) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    interval_ms_.store(interval_ms);
    timeout_ms_.store(timeout_ms);
    failure_threshold_.store(failure_threshold);
    success_threshold_.store(success_threshold);
    health_check_type_ = type;
    health_check_path_ = path;
}

void HealthChecker::start() {
    if (running_.exchange(true))
        return;
    thread_ = std::thread(&HealthChecker::run_loop, this);
}

void HealthChecker::stop() {
    if (running_.exchange(false))
        if (thread_.joinable())
            thread_.join();
}

void HealthChecker::run_loop() {
    while (running_) {
        try {
            std::vector<std::shared_ptr<lb::core::BackendNode>> backends_copy;
            {
                std::lock_guard<std::mutex> lock(backends_mutex_);
                backends_copy = backends_;
            }

            for (auto& backend : backends_copy) {
                if (!running_)
                    break;
                try {
                    check_backend(backend);
                } catch (const std::exception& e) {
                    lb::logging::Logger::instance().error(
                        "Health check failed for backend " + backend->host() + ":" +
                        std::to_string(backend->port()) + ": " + e.what());
                } catch (...) {
                    lb::logging::Logger::instance().error(
                        "Health check failed for backend " + backend->host() + ":" +
                        std::to_string(backend->port()) + ": unknown exception");
                }
            }
        } catch (const std::exception& e) {
            lb::logging::Logger::instance().error(std::string("Health checker loop exception: ") +
                                                  e.what());
        } catch (...) {
            lb::logging::Logger::instance().error(
                "Health checker loop encountered an unknown exception");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_.load()));
    }
}

bool HealthChecker::check_backend(const std::shared_ptr<lb::core::BackendNode>& backend) {
    std::string check_type;
    std::string check_path;
    uint32_t current_timeout;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        check_type = health_check_type_;
        check_path = health_check_path_;
        current_timeout = timeout_ms_.load();
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        update_backend_state(backend, false);
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(sock);
        update_backend_state(backend, false);
        return false;
    }

    struct addrinfo hints {
    }, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err =
        getaddrinfo(backend->host().c_str(), std::to_string(backend->port()).c_str(), &hints, &res);
    if (gai_err != 0 || !res) {
        ::close(sock);
        update_backend_state(backend, false);
        return false;
    }

    int result = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    bool connected = false;

    if (result == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);

        timeval timeout{};
        uint32_t current_timeout = timeout_ms_.load();
        timeout.tv_sec = current_timeout / 1000;
        timeout.tv_usec = (current_timeout % 1000) * 1000;

        int select_result = select(sock + 1, nullptr, &write_fds, nullptr, &timeout);

        if (select_result > 0 && FD_ISSET(sock, &write_fds)) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                connected = true;
        }
    }

    if (!connected) {
        ::close(sock);
        update_backend_state(backend, false);
        return false;
    }

    if (check_type != "http") {
        ::close(sock);
        update_backend_state(backend, true);
        return true;
    }

    bool http_healthy = check_http_health(sock, backend, check_path, current_timeout);
    ::close(sock);
    update_backend_state(backend, http_healthy);
    return http_healthy;
}

bool HealthChecker::check_http_health(int sock,
                                      const std::shared_ptr<lb::core::BackendNode>& backend,
                                      const std::string& path, uint32_t timeout_ms) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return false;
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << backend->host() << ":" << backend->port() << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";

    std::string request_str = request.str();
    ssize_t sent = send(sock, request_str.c_str(), request_str.size(), 0);
    if (sent != static_cast<ssize_t>(request_str.size())) {
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        return false;
    }

    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        return false;
    }

    buffer[received] = '\0';
    std::string response(buffer, received);

    size_t http_pos = response.find("HTTP/");
    if (http_pos == std::string::npos) {
        return false;
    }

    size_t space1 = response.find(' ', http_pos);
    if (space1 == std::string::npos) {
        return false;
    }

    size_t space2 = response.find(' ', space1 + 1);
    if (space2 == std::string::npos) {
        space2 = response.find('\r', space1 + 1);
        if (space2 == std::string::npos) {
            space2 = response.find('\n', space1 + 1);
        }
        if (space2 == std::string::npos) {
            return false;
        }
    }

    std::string status_str = response.substr(space1 + 1, space2 - space1 - 1);
    if (status_str.length() != 3) {
        return false;
    }

    int status_code = 0;
    for (char c : status_str) {
        if (c < '0' || c > '9') {
            return false;
        }
        status_code = status_code * 10 + (c - '0');
    }

    return status_code >= 200 && status_code < 300;
}

void HealthChecker::update_backend_state(const std::shared_ptr<lb::core::BackendNode>& backend,
                                         bool healthy) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto current_state = backend->state();

    if (healthy) {
        consecutive_failures_[backend] = 0;
        consecutive_successes_[backend]++;

        if (current_state == lb::core::BackendState::UNHEALTHY) {
            if (consecutive_successes_[backend] >= success_threshold_.load()) {
                backend->set_state(lb::core::BackendState::HEALTHY);
                consecutive_successes_[backend] = 0;
                lb::logging::Logger::instance().info("Backend " + backend->host() + ":" +
                                                     std::to_string(backend->port()) +
                                                     " transitioned to HEALTHY");
            }
        }
    } else {
        consecutive_successes_[backend] = 0;
        consecutive_failures_[backend]++;

        if (current_state == lb::core::BackendState::HEALTHY) {
            if (consecutive_failures_[backend] >= failure_threshold_.load()) {
                backend->set_state(lb::core::BackendState::UNHEALTHY);
                consecutive_failures_[backend] = 0;
                lb::logging::Logger::instance().warn("Backend " + backend->host() + ":" +
                                                     std::to_string(backend->port()) +
                                                     " transitioned to UNHEALTHY");
            }
        }
    }
}

} // namespace lb::health

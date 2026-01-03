#include "health/health_checker.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace lb::health {

HealthChecker::HealthChecker()
    : running_(false), interval_ms_(5000), timeout_ms_(500), failure_threshold_(3),
      success_threshold_(2) {}

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
        std::vector<std::shared_ptr<lb::core::BackendNode>> backends_copy;
        {
            std::lock_guard<std::mutex> lock(backends_mutex_);
            backends_copy = backends_;
        }

        for (auto& backend : backends_copy) {
            if (!running_)
                break;
            check_backend(backend);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

bool HealthChecker::check_backend(const std::shared_ptr<lb::core::BackendNode>& backend) {
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

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend->port());

    if (inet_pton(AF_INET, backend->host().c_str(), &addr.sin_addr) <= 0) {
        ::close(sock);
        update_backend_state(backend, false);
        return false;
    }

    int result = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool connected = false;

    if (result == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);

        timeval timeout{};
        timeout.tv_sec = timeout_ms_ / 1000;
        timeout.tv_usec = (timeout_ms_ % 1000) * 1000;

        int select_result = select(sock + 1, nullptr, &write_fds, nullptr, &timeout);

        if (select_result > 0 && FD_ISSET(sock, &write_fds)) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                connected = true;
        }
    }

    ::close(sock);
    update_backend_state(backend, connected);
    return connected;
}

void HealthChecker::update_backend_state(const std::shared_ptr<lb::core::BackendNode>& backend,
                                         bool healthy) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto current_state = backend->state();

    if (healthy) {
        consecutive_failures_[backend] = 0;
        consecutive_successes_[backend]++;

        if (current_state == lb::core::BackendState::UNHEALTHY) {
            if (consecutive_successes_[backend] >= success_threshold_) {
                backend->set_state(lb::core::BackendState::HEALTHY);
                consecutive_successes_[backend] = 0;
            }
        }
    } else {
        consecutive_successes_[backend] = 0;
        consecutive_failures_[backend]++;

        if (current_state == lb::core::BackendState::HEALTHY) {
            if (consecutive_failures_[backend] >= failure_threshold_) {
                backend->set_state(lb::core::BackendState::UNHEALTHY);
                consecutive_failures_[backend] = 0;
            }
        }
    }
}

} // namespace lb::health

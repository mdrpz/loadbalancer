#include "metrics/metrics_server.h"
#include "metrics/metrics.h"
#include "net/tcp_listener.h"
#include <unistd.h>
#include <sys/socket.h>
#include <string>

namespace lb::metrics {

MetricsServer::MetricsServer(uint16_t port)
    : port_(port), listener_fd_(-1), running_(false) {
}

MetricsServer::~MetricsServer() {
    stop();
}

void MetricsServer::start() {
    // TODO: Implement metrics HTTP server (Phase 3)
    // Simple TCP listener that responds to GET /metrics
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
    // TODO: Accept connections and serve metrics
}

} // namespace lb::metrics


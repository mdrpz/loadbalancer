#include "metrics/metrics.h"
#include <iomanip>
#include <mutex>
#include <sstream>

namespace lb::metrics {

Metrics& Metrics::instance() {
    static Metrics instance;
    return instance;
}

void Metrics::increment_connections_total() {
    connections_total_++;
}

void Metrics::increment_connections_active() {
    connections_active_++;
}

void Metrics::decrement_connections_active() {
    connections_active_--;
}

void Metrics::increment_backend_routed(const std::string& backend) {
    std::lock_guard<std::mutex> lock(backend_metrics_mutex_);
    backend_routed_[backend]++;
}

void Metrics::increment_backend_failures(const std::string& backend) {
    std::lock_guard<std::mutex> lock(backend_metrics_mutex_);
    backend_failures_[backend]++;
}

void Metrics::increment_backend_routes_failed() {
    backend_routes_failed_++;
}

void Metrics::increment_overload_drops() {
    overload_drops_++;
}

void Metrics::increment_request_timeouts() {
    request_timeouts_++;
}

void Metrics::increment_rate_limit_drops() {
    rate_limit_drops_++;
}

void Metrics::increment_queue_drops() {
    queue_drops_++;
}

void Metrics::increment_memory_budget_drops() {
    memory_budget_drops_++;
}

void Metrics::increment_bad_requests() {
    bad_requests_++;
}

void Metrics::add_bytes_in(uint64_t bytes) {
    bytes_in_ += bytes;
}

void Metrics::add_bytes_out(uint64_t bytes) {
    bytes_out_ += bytes;
}

void Metrics::record_request_latency_ms(double latency_ms) {
    for (size_t i = 0; i < LATENCY_BUCKETS.size(); ++i) {
        if (latency_ms <= LATENCY_BUCKETS[i]) {
            latency_bucket_counts_[i]++;
        }
    }
    latency_bucket_counts_[LATENCY_BUCKETS.size()]++;
    latency_count_++;

    double current = latency_sum_.load();
    while (!latency_sum_.compare_exchange_weak(current, current + latency_ms)) {
    }
}

uint64_t Metrics::get_connections_total() const {
    return connections_total_.load();
}

uint64_t Metrics::get_connections_active() const {
    return connections_active_.load();
}

uint64_t Metrics::get_backend_routed(const std::string& backend) const {
    std::lock_guard<std::mutex> lock(backend_metrics_mutex_);
    auto it = backend_routed_.find(backend);
    return (it != backend_routed_.end()) ? it->second.load() : 0;
}

uint64_t Metrics::get_backend_failures(const std::string& backend) const {
    std::lock_guard<std::mutex> lock(backend_metrics_mutex_);
    auto it = backend_failures_.find(backend);
    return (it != backend_failures_.end()) ? it->second.load() : 0;
}

uint64_t Metrics::get_backend_routes_failed() const {
    return backend_routes_failed_.load();
}

uint64_t Metrics::get_overload_drops() const {
    return overload_drops_.load();
}

uint64_t Metrics::get_request_timeouts() const {
    return request_timeouts_.load();
}

uint64_t Metrics::get_rate_limit_drops() const {
    return rate_limit_drops_.load();
}

uint64_t Metrics::get_queue_drops() const {
    return queue_drops_.load();
}

uint64_t Metrics::get_memory_budget_drops() const {
    return memory_budget_drops_.load();
}

uint64_t Metrics::get_bad_requests() const {
    return bad_requests_.load();
}

uint64_t Metrics::get_bytes_in() const {
    return bytes_in_.load();
}

uint64_t Metrics::get_bytes_out() const {
    return bytes_out_.load();
}

std::string Metrics::export_prometheus() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);

    oss << "# HELP lb_connections_total Total number of connections accepted\n";
    oss << "# TYPE lb_connections_total counter\n";
    oss << "lb_connections_total " << get_connections_total() << "\n\n";

    oss << "# HELP lb_connections_active Current number of active connections\n";
    oss << "# TYPE lb_connections_active gauge\n";
    oss << "lb_connections_active " << get_connections_active() << "\n\n";

    oss << "# HELP lb_backend_routes_failed_total Connections failed to route to any backend\n";
    oss << "# TYPE lb_backend_routes_failed_total counter\n";
    oss << "lb_backend_routes_failed_total " << get_backend_routes_failed() << "\n\n";

    oss << "# HELP lb_overload_drops_total Connections dropped due to overload\n";
    oss << "# TYPE lb_overload_drops_total counter\n";
    oss << "lb_overload_drops_total " << get_overload_drops() << "\n\n";

    oss << "# HELP lb_request_timeouts_total Requests that timed out\n";
    oss << "# TYPE lb_request_timeouts_total counter\n";
    oss << "lb_request_timeouts_total " << get_request_timeouts() << "\n\n";

    oss << "# HELP lb_rate_limit_drops_total Connections dropped due to rate limiting\n";
    oss << "# TYPE lb_rate_limit_drops_total counter\n";
    oss << "lb_rate_limit_drops_total " << get_rate_limit_drops() << "\n\n";

    oss << "# HELP lb_queue_drops_total Connections dropped due to queue limits/timeouts\n";
    oss << "# TYPE lb_queue_drops_total counter\n";
    oss << "lb_queue_drops_total " << get_queue_drops() << "\n\n";

    oss << "# HELP lb_memory_budget_drops_total Connections dropped due to memory budget\n";
    oss << "# TYPE lb_memory_budget_drops_total counter\n";
    oss << "lb_memory_budget_drops_total " << get_memory_budget_drops() << "\n\n";

    oss << "# HELP lb_bad_requests_total Invalid HTTP requests (400 Bad Request)\n";
    oss << "# TYPE lb_bad_requests_total counter\n";
    oss << "lb_bad_requests_total " << get_bad_requests() << "\n\n";

    oss << "# HELP lb_bytes_received_total Total bytes received from clients\n";
    oss << "# TYPE lb_bytes_received_total counter\n";
    oss << "lb_bytes_received_total " << get_bytes_in() << "\n\n";

    oss << "# HELP lb_bytes_sent_total Total bytes sent to clients\n";
    oss << "# TYPE lb_bytes_sent_total counter\n";
    oss << "lb_bytes_sent_total " << get_bytes_out() << "\n\n";

    oss << "# HELP lb_request_duration_ms Request latency in milliseconds\n";
    oss << "# TYPE lb_request_duration_ms histogram\n";
    uint64_t cumulative = 0;
    for (size_t i = 0; i < LATENCY_BUCKETS.size(); ++i) {
        cumulative += latency_bucket_counts_[i].load();
        oss << "lb_request_duration_ms_bucket{le=\"" << LATENCY_BUCKETS[i] << "\"} " << cumulative
            << "\n";
    }
    oss << "lb_request_duration_ms_bucket{le=\"+Inf\"} "
        << latency_bucket_counts_[LATENCY_BUCKETS.size()].load() << "\n";
    oss << "lb_request_duration_ms_sum " << latency_sum_.load() << "\n";
    oss << "lb_request_duration_ms_count " << latency_count_.load() << "\n\n";

    {
        std::lock_guard<std::mutex> lock(backend_metrics_mutex_);

        oss << "# HELP lb_backend_requests_total Total requests routed to backend\n";
        oss << "# TYPE lb_backend_requests_total counter\n";
        for (const auto& [backend, count] : backend_routed_) {
            oss << "lb_backend_requests_total{backend=\"" << backend << "\"} " << count.load()
                << "\n";
        }
        oss << "\n";

        oss << "# HELP lb_backend_errors_total Total backend connection errors\n";
        oss << "# TYPE lb_backend_errors_total counter\n";
        for (const auto& [backend, count] : backend_failures_) {
            oss << "lb_backend_errors_total{backend=\"" << backend << "\"} " << count.load()
                << "\n";
        }
    }

    return oss.str();
}

} // namespace lb::metrics

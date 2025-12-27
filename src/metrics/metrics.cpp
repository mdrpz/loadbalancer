#include "metrics/metrics.h"
#include <sstream>
#include <mutex>

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

void Metrics::increment_overload_drops() {
    overload_drops_++;
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

uint64_t Metrics::get_overload_drops() const {
    return overload_drops_.load();
}

std::string Metrics::export_prometheus() const {
    std::ostringstream oss;
    
    oss << "# HELP lb_connections_total Total number of connections accepted\n";
    oss << "# TYPE lb_connections_total counter\n";
    oss << "lb_connections_total " << get_connections_total() << "\n";
    
    oss << "# HELP lb_connections_active Current number of active connections\n";
    oss << "# TYPE lb_connections_active gauge\n";
    oss << "lb_connections_active " << get_connections_active() << "\n";
    
    oss << "# HELP lb_overload_drops Total number of connections dropped due to overload\n";
    oss << "# TYPE lb_overload_drops counter\n";
    oss << "lb_overload_drops " << get_overload_drops() << "\n";
    
    // Per-backend metrics
    {
        std::lock_guard<std::mutex> lock(backend_metrics_mutex_);
        for (const auto& [backend, count] : backend_routed_) {
            oss << "# HELP lb_backend_routed_total Total connections routed to backend\n";
            oss << "# TYPE lb_backend_routed_total counter\n";
            oss << "lb_backend_routed_total{backend=\"" << backend << "\"} " << count.load() << "\n";
        }
        
        for (const auto& [backend, count] : backend_failures_) {
            oss << "# HELP lb_backend_failures_total Total backend failures\n";
            oss << "# TYPE lb_backend_failures_total counter\n";
            oss << "lb_backend_failures_total{backend=\"" << backend << "\"} " << count.load() << "\n";
        }
    }
    
    return oss.str();
}

} // namespace lb::metrics


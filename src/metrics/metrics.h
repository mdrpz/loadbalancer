#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <unordered_map>

namespace lb::metrics {

class Metrics {
public:
    static Metrics& instance();

    void increment_connections_total();
    void increment_connections_active();
    void decrement_connections_active();
    void increment_backend_routed(const std::string& backend);
    void increment_backend_failures(const std::string& backend);
    void increment_backend_routes_failed();
    void increment_overload_drops();

    uint64_t get_connections_total() const;
    uint64_t get_connections_active() const;
    uint64_t get_backend_routed(const std::string& backend) const;
    uint64_t get_backend_failures(const std::string& backend) const;
    uint64_t get_backend_routes_failed() const;
    uint64_t get_overload_drops() const;

    // Export as Prometheus format
    std::string export_prometheus() const;

private:
    Metrics() = default;
    ~Metrics() = default;

    std::atomic<uint64_t> connections_total_{0};
    std::atomic<uint64_t> connections_active_{0};
    std::atomic<uint64_t> backend_routes_failed_{0};
    std::atomic<uint64_t> overload_drops_{0};
    
    // Per-backend metrics
    mutable std::mutex backend_metrics_mutex_;
    std::unordered_map<std::string, std::atomic<uint64_t>> backend_routed_;
    std::unordered_map<std::string, std::atomic<uint64_t>> backend_failures_;
};

} // namespace lb::metrics


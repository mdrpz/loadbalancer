#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
    void increment_request_timeouts();

    void add_bytes_in(uint64_t bytes);
    void add_bytes_out(uint64_t bytes);
    void record_request_latency_ms(double latency_ms);

    uint64_t get_connections_total() const;
    uint64_t get_connections_active() const;
    uint64_t get_backend_routed(const std::string& backend) const;
    uint64_t get_backend_failures(const std::string& backend) const;
    uint64_t get_backend_routes_failed() const;
    uint64_t get_overload_drops() const;
    uint64_t get_request_timeouts() const;
    uint64_t get_bytes_in() const;
    uint64_t get_bytes_out() const;

    std::string export_prometheus() const;

private:
    Metrics() = default;
    ~Metrics() = default;

    std::atomic<uint64_t> connections_total_{0};
    std::atomic<uint64_t> connections_active_{0};
    std::atomic<uint64_t> backend_routes_failed_{0};
    std::atomic<uint64_t> overload_drops_{0};
    std::atomic<uint64_t> request_timeouts_{0};
    std::atomic<uint64_t> bytes_in_{0};
    std::atomic<uint64_t> bytes_out_{0};

    static constexpr std::array<double, 10> LATENCY_BUCKETS = {1.0,   5.0,   10.0,  25.0,   50.0,
                                                               100.0, 250.0, 500.0, 1000.0, 5000.0};
    mutable std::mutex latency_mutex_;
    std::array<std::atomic<uint64_t>, 11> latency_bucket_counts_{};
    std::atomic<uint64_t> latency_count_{0};
    std::atomic<double> latency_sum_{0.0};

    mutable std::mutex backend_metrics_mutex_;
    std::unordered_map<std::string, std::atomic<uint64_t>> backend_routed_;
    std::unordered_map<std::string, std::atomic<uint64_t>> backend_failures_;
};

} // namespace lb::metrics

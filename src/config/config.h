#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>

namespace lb::config {

struct BackendConfig {
    std::string host;
    uint16_t port;
};

struct Config {
    // Listener
    std::string listen_host;
    uint16_t listen_port;
    bool tls_enabled;
    std::string tls_cert_path;
    std::string tls_key_path;

    // Backends
    std::vector<BackendConfig> backends;

    // Routing
    std::string routing_algorithm; // "round_robin" or "least_connections"
    uint32_t max_connections_per_backend;
    uint32_t max_global_connections;

    // Health check
    uint32_t health_check_interval_ms;
    uint32_t health_check_timeout_ms;
    uint32_t health_check_failure_threshold;
    uint32_t health_check_success_threshold;
    std::string health_check_type; // "tcp" or "http"

    // Thread pool
    uint32_t thread_pool_worker_count;

    // Metrics
    bool metrics_enabled;
    uint16_t metrics_port;

    // Logging
    std::string log_level;
    std::string log_file;

    // Memory
    uint32_t global_buffer_budget_mb;

    // Backpressure
    uint32_t backpressure_timeout_ms;

    // Graceful shutdown
    uint32_t graceful_shutdown_timeout_seconds;
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    bool load_from_file(const std::string& path);
    std::shared_ptr<const Config> get_config() const;

    // Hot reload support (Phase 3)
    void start_reload_watcher();
    void stop_reload_watcher();

private:
    std::shared_ptr<Config> config_;
    std::string config_path_;
};

} // namespace lb::config


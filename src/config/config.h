#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace lb::config {

struct BackendConfig {
    std::string host;
    uint16_t port;
    uint32_t weight = 1;
};

struct Config {
    std::string listen_host;
    uint16_t listen_port;
    bool tls_enabled;
    std::string tls_cert_path;
    std::string tls_key_path;

    std::string mode; // "tcp" or "http"
    bool use_splice;  // Use zero-copy splice() for TCP mode (Linux only)

    std::vector<BackendConfig> backends;

    std::string routing_algorithm; // "round_robin" or "least_connections"
    uint32_t max_connections_per_backend;
    uint32_t max_global_connections;

    bool connection_pool_enabled;
    uint32_t pool_min_connections;    // Minimum idle connections per backend
    uint32_t pool_max_connections;    // Maximum connections per backend
    uint32_t pool_idle_timeout_ms;    // Close idle connections after this time
    uint32_t pool_connect_timeout_ms; // Timeout for new pool connections

    uint32_t health_check_interval_ms;
    uint32_t health_check_timeout_ms;
    uint32_t health_check_failure_threshold;
    uint32_t health_check_success_threshold;
    std::string health_check_type; // "tcp" or "http"

    uint32_t thread_pool_worker_count;

    bool metrics_enabled;
    uint16_t metrics_port;

    std::string log_level;
    std::string log_file;

    uint32_t global_buffer_budget_mb;

    uint32_t backpressure_timeout_ms;

    uint32_t graceful_shutdown_timeout_seconds;
};

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    bool load_from_file(const std::string& path);
    [[nodiscard]] std::shared_ptr<const Config> get_config() const;

    void start_reload_watcher();
    void stop_reload_watcher();

    bool check_and_reload();

private:
    std::shared_ptr<Config> config_;
    std::string config_path_;
    std::time_t last_modified_time_;
    bool yaml_cpp_warning_shown_;

    [[nodiscard]] static std::time_t get_file_mtime(const std::string& path);
};

} // namespace lb::config

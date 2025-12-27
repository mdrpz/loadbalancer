#include "config/config.h"

namespace lb::config {

ConfigManager::ConfigManager() {
    // Initialize with defaults
    config_ = std::make_shared<Config>();
    config_->listen_host = "0.0.0.0";
    config_->listen_port = 8080;
    config_->tls_enabled = false;
    config_->max_connections_per_backend = 100;
    config_->max_global_connections = 1000;
    config_->health_check_interval_ms = 5000;
    config_->health_check_timeout_ms = 500;
    config_->health_check_failure_threshold = 3;
    config_->health_check_success_threshold = 2;
    config_->health_check_type = "tcp";
    config_->thread_pool_worker_count = 4;
    config_->metrics_enabled = true;
    config_->metrics_port = 9090;
    config_->log_level = "info";
    config_->global_buffer_budget_mb = 512;
    config_->backpressure_timeout_ms = 10000;
    config_->graceful_shutdown_timeout_seconds = 30;
}

ConfigManager::~ConfigManager() = default;

bool ConfigManager::load_from_file(const std::string& path) {
    // TODO: Implement YAML parsing (Phase 3)
    config_path_ = path;
    return false; // Not implemented yet
}

std::shared_ptr<const Config> ConfigManager::get_config() const {
    return config_;
}

void ConfigManager::start_reload_watcher() {
    // TODO: Implement config file watching (Phase 3)
}

void ConfigManager::stop_reload_watcher() {
    // TODO: Implement config file watching (Phase 3)
}

} // namespace lb::config


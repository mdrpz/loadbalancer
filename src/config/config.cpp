#include "config/config.h"
#ifdef HAVE_YAML_CPP
#include <yaml-cpp/yaml.h>
#endif
#include <sys/stat.h>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace lb::config {

ConfigManager::ConfigManager() : last_modified_time_(0), yaml_cpp_warning_shown_(false) {
    config_ = std::make_shared<Config>();
    config_->listen_host = "0.0.0.0";
    config_->listen_port = 8080;
    config_->tls_enabled = false;
    config_->mode = "tcp";
    config_->use_splice = false;
    config_->routing_algorithm = "round_robin";
    config_->max_connections_per_backend = 100;
    config_->max_global_connections = 1000;
    config_->connection_pool_enabled = true;
    config_->pool_min_connections = 0;
    config_->pool_max_connections = 10;
    config_->pool_idle_timeout_ms = 60000;
    config_->pool_connect_timeout_ms = 5000;
    config_->health_check_interval_ms = 5000;
    config_->health_check_timeout_ms = 500;
    config_->health_check_failure_threshold = 3;
    config_->health_check_success_threshold = 2;
    config_->health_check_type = "tcp";
    config_->thread_pool_worker_count = 4;
    config_->metrics_enabled = true;
    config_->metrics_port = 9090;
    config_->log_level = "info";
    config_->access_log_enabled = false;
    config_->request_timeout_ms = 30000;
    config_->global_buffer_budget_mb = 512;
    config_->backpressure_timeout_ms = 10000;
    config_->graceful_shutdown_timeout_seconds = 30;
    config_->ip_whitelist.clear();
    config_->ip_blacklist.clear();
    config_->http_request_headers_add.clear();
    config_->http_request_headers_remove.clear();
    config_->http_response_headers_add.clear();
    config_->http_response_headers_remove.clear();
    config_->rate_limit_enabled = false;
    config_->rate_limit_max_connections = 100;
    config_->rate_limit_window_seconds = 60;
    config_->queue_enabled = false;
    config_->queue_max_size = 0;
    config_->queue_max_wait_ms = 0;
}

ConfigManager::~ConfigManager() = default;

bool ConfigManager::load_from_file(const std::string& path) {
#ifdef HAVE_YAML_CPP
    try {
        YAML::Node config = YAML::LoadFile(path);

        auto new_config = config_ ? std::make_shared<Config>(*config_) : std::make_shared<Config>();

        if (!config_) {
            ConfigManager temp;
            *new_config = *temp.config_;
        }

        if (config["listener"]) {
            const auto& listener = config["listener"];
            if (listener["host"])
                new_config->listen_host = listener["host"].as<std::string>();
            if (listener["port"])
                new_config->listen_port = listener["port"].as<uint16_t>();
            if (listener["tls_enabled"])
                new_config->tls_enabled = listener["tls_enabled"].as<bool>();
            if (listener["tls_cert"]) {
                auto cert_path = listener["tls_cert"].as<std::string>();
                if (!cert_path.empty() && !path.empty()) {
                    try {
                        std::filesystem::path cert_file(cert_path);
                        if (!cert_file.is_absolute()) {
                            std::filesystem::path config_file(path);
                            auto config_dir = config_file.parent_path();
                            if (!config_dir.empty()) {
                                new_config->tls_cert_path = (config_dir / cert_path).string();
                            } else {
                                new_config->tls_cert_path = cert_path;
                            }
                        } else {
                            new_config->tls_cert_path = cert_path;
                        }
                    } catch (const std::exception&) {
                        new_config->tls_cert_path = cert_path;
                    }
                } else {
                    new_config->tls_cert_path = cert_path;
                }
            }
            if (listener["tls_key"]) {
                auto key_path = listener["tls_key"].as<std::string>();
                if (!key_path.empty() && !path.empty()) {
                    try {
                        std::filesystem::path key_file(key_path);
                        if (!key_file.is_absolute()) {
                            std::filesystem::path config_file(path);
                            auto config_dir = config_file.parent_path();
                            if (!config_dir.empty()) {
                                new_config->tls_key_path = (config_dir / key_path).string();
                            } else {
                                new_config->tls_key_path = key_path;
                            }
                        } else {
                            new_config->tls_key_path = key_path;
                        }
                    } catch (const std::exception&) {
                        new_config->tls_key_path = key_path;
                    }
                } else {
                    new_config->tls_key_path = key_path;
                }
            }
            if (listener["mode"])
                new_config->mode = listener["mode"].as<std::string>();
            if (listener["use_splice"])
                new_config->use_splice = listener["use_splice"].as<bool>();
        }

        if (config["backends"] && config["backends"].IsSequence()) {
            new_config->backends.clear();
            for (const auto& backend : config["backends"]) {
                BackendConfig backend_cfg;
                if (backend["host"])
                    backend_cfg.host = backend["host"].as<std::string>();
                if (backend["port"])
                    backend_cfg.port = backend["port"].as<uint16_t>();
                if (backend["weight"])
                    backend_cfg.weight = backend["weight"].as<uint32_t>();
                new_config->backends.push_back(backend_cfg);
            }
        }

        if (config["routing"]) {
            const auto& routing = config["routing"];
            if (routing["algorithm"])
                new_config->routing_algorithm = routing["algorithm"].as<std::string>();
            if (routing["max_connections_per_backend"])
                new_config->max_connections_per_backend =
                    routing["max_connections_per_backend"].as<uint32_t>();
            if (routing["max_global_connections"])
                new_config->max_global_connections =
                    routing["max_global_connections"].as<uint32_t>();
        }

        if (config["connection_pool"]) {
            const auto& pool = config["connection_pool"];
            if (pool["enabled"])
                new_config->connection_pool_enabled = pool["enabled"].as<bool>();
            if (pool["min_connections"])
                new_config->pool_min_connections = pool["min_connections"].as<uint32_t>();
            if (pool["max_connections"])
                new_config->pool_max_connections = pool["max_connections"].as<uint32_t>();
            if (pool["idle_timeout_ms"])
                new_config->pool_idle_timeout_ms = pool["idle_timeout_ms"].as<uint32_t>();
            if (pool["connect_timeout_ms"])
                new_config->pool_connect_timeout_ms = pool["connect_timeout_ms"].as<uint32_t>();
        }

        if (config["health_check"]) {
            const auto& health = config["health_check"];
            if (health["interval_ms"])
                new_config->health_check_interval_ms = health["interval_ms"].as<uint32_t>();
            if (health["timeout_ms"])
                new_config->health_check_timeout_ms = health["timeout_ms"].as<uint32_t>();
            if (health["failure_threshold"])
                new_config->health_check_failure_threshold =
                    health["failure_threshold"].as<uint32_t>();
            if (health["success_threshold"])
                new_config->health_check_success_threshold =
                    health["success_threshold"].as<uint32_t>();
            if (health["type"])
                new_config->health_check_type = health["type"].as<std::string>();
        }

        if (config["thread_pool"]) {
            const auto& thread_pool = config["thread_pool"];
            if (thread_pool["worker_count"])
                new_config->thread_pool_worker_count = thread_pool["worker_count"].as<uint32_t>();
        }

        if (config["metrics"]) {
            const auto& metrics = config["metrics"];
            if (metrics["enabled"])
                new_config->metrics_enabled = metrics["enabled"].as<bool>();
            if (metrics["port"])
                new_config->metrics_port = metrics["port"].as<uint16_t>();
        }

        if (config["logging"]) {
            const auto& logging = config["logging"];
            if (logging["level"])
                new_config->log_level = logging["level"].as<std::string>();
            if (logging["file"])
                new_config->log_file = logging["file"].as<std::string>();
            if (logging["access_log_enabled"])
                new_config->access_log_enabled = logging["access_log_enabled"].as<bool>();
            if (logging["access_log_file"])
                new_config->access_log_file = logging["access_log_file"].as<std::string>();
        }

        if (config["timeouts"]) {
            const auto& timeouts = config["timeouts"];
            if (timeouts["request_ms"])
                new_config->request_timeout_ms = timeouts["request_ms"].as<uint32_t>();
        }

        if (config["memory"]) {
            const auto& memory = config["memory"];
            if (memory["global_buffer_budget_mb"])
                new_config->global_buffer_budget_mb =
                    memory["global_buffer_budget_mb"].as<uint32_t>();
        }

        if (config["backpressure"]) {
            const auto& backpressure = config["backpressure"];
            if (backpressure["timeout_ms"])
                new_config->backpressure_timeout_ms = backpressure["timeout_ms"].as<uint32_t>();
        }

        if (config["graceful_shutdown"]) {
            const auto& shutdown = config["graceful_shutdown"];
            if (shutdown["timeout_seconds"])
                new_config->graceful_shutdown_timeout_seconds =
                    shutdown["timeout_seconds"].as<uint32_t>();
        }

        if (config["ip_filter"]) {
            const auto& ip_filter = config["ip_filter"];
            if (ip_filter["whitelist"] && ip_filter["whitelist"].IsSequence()) {
                new_config->ip_whitelist.clear();
                for (const auto& ip : ip_filter["whitelist"]) {
                    new_config->ip_whitelist.push_back(ip.as<std::string>());
                }
            }
            if (ip_filter["blacklist"] && ip_filter["blacklist"].IsSequence()) {
                new_config->ip_blacklist.clear();
                for (const auto& ip : ip_filter["blacklist"]) {
                    new_config->ip_blacklist.push_back(ip.as<std::string>());
                }
            }
        }

        if (config["http_headers"] && config["http_headers"]["request"]) {
            const auto& request_headers = config["http_headers"]["request"];

            if (request_headers["add"] && request_headers["add"].IsMap()) {
                new_config->http_request_headers_add.clear();
                for (const auto& item : request_headers["add"]) {
                    std::string key = item.first.as<std::string>();
                    std::string value = item.second.as<std::string>();
                    new_config->http_request_headers_add[key] = value;
                }
            }

            if (request_headers["remove"] && request_headers["remove"].IsSequence()) {
                new_config->http_request_headers_remove.clear();
                for (const auto& header : request_headers["remove"]) {
                    new_config->http_request_headers_remove.push_back(header.as<std::string>());
                }
            }
        }

        if (config["http_headers"] && config["http_headers"]["response"]) {
            const auto& response_headers = config["http_headers"]["response"];

            if (response_headers["add"] && response_headers["add"].IsMap()) {
                new_config->http_response_headers_add.clear();
                for (const auto& item : response_headers["add"]) {
                    std::string key = item.first.as<std::string>();
                    std::string value = item.second.as<std::string>();
                    new_config->http_response_headers_add[key] = value;
                }
            }

            if (response_headers["remove"] && response_headers["remove"].IsSequence()) {
                new_config->http_response_headers_remove.clear();
                for (const auto& header : response_headers["remove"]) {
                    new_config->http_response_headers_remove.push_back(header.as<std::string>());
                }
            }
        }

        if (config["rate_limit"]) {
            const auto& rate_limit = config["rate_limit"];
            if (rate_limit["enabled"])
                new_config->rate_limit_enabled = rate_limit["enabled"].as<bool>();
            if (rate_limit["max_connections"])
                new_config->rate_limit_max_connections =
                    rate_limit["max_connections"].as<uint32_t>();
            if (rate_limit["window_seconds"])
                new_config->rate_limit_window_seconds = rate_limit["window_seconds"].as<uint32_t>();
        }

        if (config["queue"]) {
            const auto& queue = config["queue"];
            if (queue["enabled"])
                new_config->queue_enabled = queue["enabled"].as<bool>();
            if (queue["max_size"])
                new_config->queue_max_size = queue["max_size"].as<uint32_t>();
            if (queue["max_wait_ms"])
                new_config->queue_max_wait_ms = queue["max_wait_ms"].as<uint32_t>();
        }

        config_ = new_config;
        config_path_ = path;
        last_modified_time_ = get_file_mtime(path);
        return true;
    } catch (const YAML::Exception& e) {
        std::cerr << "YAML parsing error in " << path << ": " << e.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error loading config from " << path << ": " << e.what() << std::endl;
        return false;
    }
#else
    if (!yaml_cpp_warning_shown_) {
        std::cerr << "Warning: yaml-cpp not available. Install libyaml-cpp-dev and rebuild to "
                     "enable config file support."
                  << std::endl;
        yaml_cpp_warning_shown_ = true;
    }
    (void)path;
    return false;
#endif
}

std::shared_ptr<const Config> ConfigManager::get_config() const {
    return config_;
}

void ConfigManager::start_reload_watcher() {
    if (!config_path_.empty())
        last_modified_time_ = get_file_mtime(config_path_);
}

void ConfigManager::stop_reload_watcher() {}

bool ConfigManager::check_and_reload() {
    if (config_path_.empty())
        return false;

#ifndef HAVE_YAML_CPP
    return false;
#endif

    std::time_t current_mtime = get_file_mtime(config_path_);
    if (current_mtime > last_modified_time_)
        return load_from_file(config_path_);
    return false;
}

std::time_t ConfigManager::get_file_mtime(const std::string& path) {
    std::ifstream file(path);
    if (!file.good())
        return 0;
    file.close();

    struct stat file_stat;
    if (stat(path.c_str(), &file_stat) == 0)
        return file_stat.st_mtime;
    return 0;
}

} // namespace lb::config

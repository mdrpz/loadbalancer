#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include "config/config.h"

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        auto base_path = fs::temp_directory_path() / ("lb_test_config_" + std::to_string(std::time(nullptr)));
        temp_path_ = base_path;
        fs::create_directories(temp_path_);
    }

    ~TempDir() {
        if (fs::exists(temp_path_)) {
            fs::remove_all(temp_path_);
        }
    }

    [[nodiscard]] fs::path path() const {
        return temp_path_;
    }

    [[nodiscard]] fs::path file(const std::string& name) const {
        return temp_path_ / name;
    }

private:
    fs::path temp_path_;
};

void test_config_defaults() {
    std::cout << "Testing config defaults...\n";

    lb::config::ConfigManager mgr;
    auto config = mgr.get_config();

    assert(config != nullptr);
    assert(config->listen_host == "0.0.0.0");
    assert(config->listen_port == 8080);
    assert(config->tls_enabled == false);
    assert(config->mode == "tcp");
    assert(config->routing_algorithm == "round_robin");
    assert(config->max_connections_per_backend == 100);
    assert(config->max_global_connections == 1000);
    assert(config->health_check_interval_ms == 5000);
    assert(config->health_check_timeout_ms == 500);
    assert(config->health_check_failure_threshold == 3);
    assert(config->health_check_success_threshold == 2);
    assert(config->health_check_type == "tcp");
    assert(config->health_check_path == "/health");
    assert(config->thread_pool_worker_count == 4);
    assert(config->metrics_enabled == true);
    assert(config->metrics_port == 9090);
    assert(config->log_level == "info");
    assert(config->request_timeout_ms == 30000);
    assert(config->global_buffer_budget_mb == 512);
    assert(config->backpressure_timeout_ms == 10000);
    assert(config->graceful_shutdown_timeout_seconds == 30);
    assert(config->tls_handshake_timeout_ms == 10000);

    std::cout << "Config defaults test passed!\n";
}

void test_config_yaml_override() {
    std::cout << "Testing YAML config override...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  host: 127.0.0.1\n"
      << "  port: 9999\n"
      << "  tls_enabled: true\n"
      << "backends:\n"
      << "  - host: 10.0.0.1\n"
      << "    port: 8000\n"
      << "    weight: 5\n"
      << "  - host: 10.0.0.2\n"
      << "    port: 8001\n"
      << "routing:\n"
      << "  algorithm: least_connections\n"
      << "  max_connections_per_backend: 200\n"
      << "  max_global_connections: 2000\n"
      << "health_check:\n"
      << "  interval_ms: 10000\n"
      << "  timeout_ms: 1000\n"
      << "  failure_threshold: 5\n"
      << "  success_threshold: 3\n"
      << "  type: http\n"
      << "  path: /custom-health\n"
      << "metrics:\n"
      << "  enabled: false\n"
      << "  port: 9999\n"
      << "logging:\n"
      << "  level: debug\n"
      << "memory:\n"
      << "  global_buffer_budget_mb: 256\n";
    f.close();

    lb::config::ConfigManager mgr;
    bool loaded = mgr.load_from_file(config_file.string());
    assert(loaded == true);

    auto config = mgr.get_config();
    assert(config != nullptr);
    assert(config->listen_host == "127.0.0.1");
    assert(config->listen_port == 9999);
    assert(config->tls_enabled == true);
    assert(config->routing_algorithm == "least_connections");
    assert(config->max_connections_per_backend == 200);
    assert(config->max_global_connections == 2000);
    assert(config->health_check_interval_ms == 10000);
    assert(config->health_check_timeout_ms == 1000);
    assert(config->health_check_failure_threshold == 5);
    assert(config->health_check_success_threshold == 3);
    assert(config->health_check_type == "http");
    assert(config->health_check_path == "/custom-health");
    assert(config->metrics_enabled == false);
    assert(config->metrics_port == 9999);
    assert(config->log_level == "debug");
    assert(config->global_buffer_budget_mb == 256);
    assert(config->backends.size() == 2);
    assert(config->backends[0].host == "10.0.0.1");
    assert(config->backends[0].port == 8000);
    assert(config->backends[0].weight == 5);
    assert(config->backends[1].host == "10.0.0.2");
    assert(config->backends[1].port == 8001);
    assert(config->backends[1].weight == 1);

    std::cout << "YAML config override test passed!\n";
}

void test_config_relative_paths() {
    std::cout << "Testing relative cert/key path resolution...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");
    auto cert_file = tmp.file("server.crt");
    auto key_file = tmp.file("server.key");

    std::ofstream(cert_file) << "dummy cert\n";
    std::ofstream(key_file) << "dummy key\n";

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  tls_enabled: true\n"
      << "  tls_cert: server.crt\n"
      << "  tls_key: server.key\n";
    f.close();

    lb::config::ConfigManager mgr;
    bool loaded = mgr.load_from_file(config_file.string());
    assert(loaded == true);

    auto config = mgr.get_config();
    assert(config != nullptr);
    assert(config->tls_enabled == true);
    assert(config->tls_cert_path == cert_file.string());
    assert(config->tls_key_path == key_file.string());

    std::cout << "Relative path resolution test passed!\n";
}

void test_config_absolute_paths() {
    std::cout << "Testing absolute cert/key paths...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");
    auto cert_file = tmp.file("server.crt");
    auto key_file = tmp.file("server.key");

    std::ofstream(cert_file) << "dummy cert\n";
    std::ofstream(key_file) << "dummy key\n";

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  tls_enabled: true\n"
      << "  tls_cert: " << cert_file.string() << "\n"
      << "  tls_key: " << key_file.string() << "\n";
    f.close();

    lb::config::ConfigManager mgr;
    bool loaded = mgr.load_from_file(config_file.string());
    assert(loaded == true);

    auto config = mgr.get_config();
    assert(config != nullptr);
    assert(config->tls_cert_path == cert_file.string());
    assert(config->tls_key_path == key_file.string());

    std::cout << "Absolute path test passed!\n";
}

void test_config_invalid_yaml() {
    std::cout << "Testing invalid YAML rejection...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  host: 127.0.0.1\n"
      << "  port: [invalid: yaml\n"
      << "backends:\n";
    f.close();

    lb::config::ConfigManager mgr;
    auto initial_config_file = tmp.file("initial.yaml");
    std::ofstream init_f(initial_config_file);
    init_f << "listener:\n"
           << "  host: 192.168.1.1\n"
           << "  port: 8888\n";
    init_f.close();

    bool initial_loaded = mgr.load_from_file(initial_config_file.string());
    assert(initial_loaded == true);
    auto initial_config = mgr.get_config();
    assert(initial_config->listen_host == "192.168.1.1");
    assert(initial_config->listen_port == 8888);

    bool invalid_loaded = mgr.load_from_file(config_file.string());
    assert(invalid_loaded == false);

    auto config_after_invalid = mgr.get_config();
    assert(config_after_invalid->listen_host == "192.168.1.1");
    assert(config_after_invalid->listen_port == 8888);

    std::cout << "Invalid YAML rejection test passed!\n";
}

void test_config_hot_reload() {
    std::cout << "Testing hot reload with malformed update...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  host: 10.0.0.1\n"
      << "  port: 7777\n";
    f.close();

    lb::config::ConfigManager mgr;
    bool loaded = mgr.load_from_file(config_file.string());
    assert(loaded == true);
    auto initial_config = mgr.get_config();
    assert(initial_config->listen_host == "10.0.0.1");
    assert(initial_config->listen_port == 7777);

    mgr.start_reload_watcher();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::ofstream f2(config_file);
    f2 << "listener:\n"
       << "  host: 10.0.0.2\n"
       << "  port: [invalid\n";
    f2.close();

    bool reloaded = mgr.check_and_reload();
    assert(reloaded == false);

    auto config_after_reload = mgr.get_config();
    assert(config_after_reload->listen_host == "10.0.0.1");
    assert(config_after_reload->listen_port == 7777);

    std::cout << "Hot reload malformed update test passed!\n";
}

void test_config_hot_reload_valid() {
    std::cout << "Testing hot reload with valid update...\n";

    TempDir tmp;
    auto config_file = tmp.file("config.yaml");

    std::ofstream f(config_file);
    f << "listener:\n"
      << "  host: 10.0.0.1\n"
      << "  port: 7777\n";
    f.close();

    lb::config::ConfigManager mgr;
    bool loaded = mgr.load_from_file(config_file.string());
    assert(loaded == true);

    mgr.start_reload_watcher();

    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    std::ofstream f2(config_file);
    f2 << "listener:\n"
       << "  host: 10.0.0.2\n"
       << "  port: 8888\n";
    f2.close();

    bool reloaded = mgr.check_and_reload();
    assert(reloaded == true);

    auto config_after_reload = mgr.get_config();
    assert(config_after_reload->listen_host == "10.0.0.2");
    assert(config_after_reload->listen_port == 8888);

    std::cout << "Hot reload valid update test passed!\n";
}

int main() {
    try {
        test_config_defaults();
        test_config_yaml_override();
        test_config_relative_paths();
        test_config_absolute_paths();
        test_config_invalid_yaml();
        test_config_hot_reload();
        test_config_hot_reload_valid();
        std::cout << "\nAll config tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
}

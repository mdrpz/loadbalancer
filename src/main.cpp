#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include "config/config.h"
#include "core/load_balancer.h"
#include "logging/access_logger.h"
#include "logging/logger.h"
#include "metrics/metrics_server.h"

namespace {
std::atomic<bool> g_shutdown{false};
lb::core::LoadBalancer* g_lb = nullptr;

void signal_handler(int sig) {
    (void)sig;
    g_shutdown.store(true, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    lb::config::ConfigManager config_manager;
    std::string config_path;

    bool config_loaded = false;
    if (argc >= 2) {
        config_path = argv[1];
        config_loaded = config_manager.load_from_file(config_path);
        if (!config_loaded) {
#ifndef HAVE_YAML_CPP
#else
            std::cerr << "Failed to load config from: " << config_path << "\n";
#endif
            std::cerr << "Using command-line arguments or defaults\n";
        } else {
            std::cout << "Loaded config from: " << config_path << "\n";
            config_manager.start_reload_watcher();
        }
    }

    auto config = config_manager.get_config();

    auto& logger = lb::logging::Logger::instance();
    if (config && !config->log_file.empty())
        logger.set_output_file(config->log_file);
    if (config) {
        std::string level = config->log_level;
        if (level == "debug" || level == "DEBUG")
            logger.set_level(lb::logging::LogLevel::DEBUG);
        else if (level == "warn" || level == "WARN")
            logger.set_level(lb::logging::LogLevel::WARN);
        else if (level == "error" || level == "ERROR")
            logger.set_level(lb::logging::LogLevel::ERROR);
        else
            logger.set_level(lb::logging::LogLevel::INFO);
    }
    logger.start();
    logger.info("Load balancer starting");

    auto& access_logger = lb::logging::AccessLogger::instance();
    if (config && config->access_log_enabled) {
        access_logger.set_enabled(true);
        if (!config->access_log_file.empty())
            access_logger.set_output_file(config->access_log_file);
        access_logger.start();
        logger.info("Access logging enabled");
    }

    lb::core::LoadBalancer lb;
    g_lb = &lb;

    if (!config_path.empty())
        lb.set_config_manager(&config_manager);

    bool initialized = false;
    if (config_loaded && config) {
        initialized = lb.initialize_from_config(config);
    } else {
        std::string listen_host = "0.0.0.0";
        uint16_t listen_port = 8080;

        if (argc >= 2 && !config_path.empty()) {
            if (argc >= 3)
                listen_port = static_cast<uint16_t>(std::stoi(argv[2]));
            if (argc >= 4)
                listen_host = argv[3];
            if (argc >= 5) {
                std::string backends_str = argv[4];
                size_t pos = 0;
                while ((pos = backends_str.find(',')) != std::string::npos) {
                    std::string backend = backends_str.substr(0, pos);
                    size_t colon = backend.find(':');
                    if (colon != std::string::npos) {
                        std::string host = backend.substr(0, colon);
                        uint16_t port = static_cast<uint16_t>(std::stoi(backend.substr(colon + 1)));
                        lb.add_backend(host, port);
                    }
                    backends_str.erase(0, pos + 1);
                }
                size_t colon = backends_str.find(':');
                if (colon != std::string::npos) {
                    std::string host = backends_str.substr(0, colon);
                    uint16_t port =
                        static_cast<uint16_t>(std::stoi(backends_str.substr(colon + 1)));
                    lb.add_backend(host, port);
                }
            }
        } else {
            if (argc >= 2)
                listen_port = static_cast<uint16_t>(std::stoi(argv[1]));
            if (argc >= 3)
                listen_host = argv[2];
            if (argc >= 4) {
                std::string backends_str = argv[3];
                size_t pos = 0;
                while ((pos = backends_str.find(',')) != std::string::npos) {
                    std::string backend = backends_str.substr(0, pos);
                    size_t colon = backend.find(':');
                    if (colon != std::string::npos) {
                        std::string host = backend.substr(0, colon);
                        uint16_t port = static_cast<uint16_t>(std::stoi(backend.substr(colon + 1)));
                        lb.add_backend(host, port);
                    }
                    backends_str.erase(0, pos + 1);
                }
                size_t colon = backends_str.find(':');
                if (colon != std::string::npos) {
                    std::string host = backends_str.substr(0, colon);
                    uint16_t port =
                        static_cast<uint16_t>(std::stoi(backends_str.substr(colon + 1)));
                    lb.add_backend(host, port);
                }
            }
        }

        initialized = lb.initialize(listen_host, listen_port);

        if (!initialized) {
            std::cerr << "Failed to initialize load balancer\n";
            return 1;
        }
    }

    if (!initialized) {
        std::cerr << "Failed to initialize load balancer\n";
        return 1;
    }

    std::atomic<bool> stop_shutdown_watcher{false};
    std::thread shutdown_watcher([&]() {
        while (!stop_shutdown_watcher.load(std::memory_order_relaxed)) {
            if (g_shutdown.load(std::memory_order_relaxed)) {
                if (g_lb)
                    g_lb->shutdown_gracefully();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    uint16_t metrics_port = config ? config->metrics_port : 9090;
    bool metrics_enabled = config ? config->metrics_enabled : true;
    std::unique_ptr<lb::metrics::MetricsServer> metrics_server;
    if (metrics_enabled) {
        metrics_server = std::make_unique<lb::metrics::MetricsServer>(metrics_port);
        metrics_server->start();
        std::cout << "Metrics server listening on port " << metrics_port << "\n";
    }

    std::cout << "Load Balancer listening on " << (config ? config->listen_host : "0.0.0.0") << ":"
              << (config ? config->listen_port : 8080) << "\n";
    std::cout << "Press Ctrl+C to stop...\n";

    lb.run();

    stop_shutdown_watcher.store(true, std::memory_order_relaxed);
    if (shutdown_watcher.joinable())
        shutdown_watcher.join();

    std::cout << "Load Balancer shutting down...\n";
    logger.info("Load balancer shutting down");
    access_logger.stop();
    logger.stop();
    return 0;
}

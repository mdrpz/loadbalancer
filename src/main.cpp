#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include "core/load_balancer.h"

namespace {
    std::atomic<bool> g_shutdown{false};
    lb::core::LoadBalancer* g_lb = nullptr;

    void signal_handler(int sig) {
        (void)sig;
        g_shutdown.store(true);
        if (g_lb) {
            g_lb->stop();
        }
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::string listen_host = "0.0.0.0";
    uint16_t listen_port = 8080;
    
    if (argc >= 2) {
        listen_port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    if (argc >= 3) {
        listen_host = argv[2];
    }

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create and initialize load balancer
    lb::core::LoadBalancer lb;
    g_lb = &lb;

    // Add some default backends (for Phase 1 testing)
    // In Phase 3, these will come from config file
    if (argc >= 4) {
        // Parse backends from command line: host1:port1,host2:port2,...
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
        // Last backend
        size_t colon = backends_str.find(':');
        if (colon != std::string::npos) {
            std::string host = backends_str.substr(0, colon);
            uint16_t port = static_cast<uint16_t>(std::stoi(backends_str.substr(colon + 1)));
            lb.add_backend(host, port);
        }
    } else {
        // Default: no backends (user must provide via command line)
        std::cerr << "Usage: " << argv[0] << " [port] [host] [backend1:port1,backend2:port2,...]\n";
        std::cerr << "Example: " << argv[0] << " 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001\n";
        return 1;
    }

    // Initialize load balancer
    if (!lb.initialize(listen_host, listen_port)) {
        std::cerr << "Failed to initialize load balancer on " << listen_host 
                  << ":" << listen_port << "\n";
        return 1;
    }

    std::cout << "Load Balancer listening on " << listen_host << ":" << listen_port << "\n";
    std::cout << "Press Ctrl+C to stop...\n";

    // Run event loop (blocks until stop() is called)
    lb.run();

    std::cout << "Load Balancer shutting down...\n";
    return 0;
}


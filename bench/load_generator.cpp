#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

struct Stats {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> successful{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> connect_errors{0};

    std::mutex latency_mutex;
    std::vector<uint64_t> latencies_us;

    void record_latency(uint64_t us) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        latencies_us.push_back(us);
    }
};

struct Config {
    std::string host;
    int port;
    int connections;
    int duration_seconds;
    bool http_mode;
};

int connect_to_server(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

bool send_http_request(int fd, const std::string& host, int port) {
    std::string request = "GET / HTTP/1.1\r\n"
                          "Host: " +
                          host + ":" + std::to_string(port) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n";

    ssize_t sent = send(fd, request.c_str(), request.size(), 0);
    return sent == static_cast<ssize_t>(request.size());
}

bool send_tcp_data(int fd) {
    const char* data = "PING\n";
    ssize_t sent = send(fd, data, strlen(data), 0);
    return sent == static_cast<ssize_t>(strlen(data));
}

bool receive_response(int fd) {
    char buffer[4096];
    ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    return received > 0;
}

void worker(const Config& config, Stats& stats, std::atomic<bool>& running, int worker_id) {
    (void)worker_id;

    while (running.load()) {
        int fd = connect_to_server(config.host, config.port);
        if (fd < 0) {
            stats.connect_errors++;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto start = std::chrono::high_resolution_clock::now();

        bool success = false;
        if (config.http_mode) {
            if (send_http_request(fd, config.host, config.port)) {
                success = receive_response(fd);
            }
        } else {
            if (send_tcp_data(fd)) {
                success = receive_response(fd);
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        stats.requests++;
        if (success) {
            stats.successful++;
            stats.record_latency(duration_us);
        } else {
            stats.failed++;
        }

        close(fd);
    }
}

void print_results(const Config& config, Stats& stats, double elapsed_seconds) {
    std::cout << "\n--- Results ---\n";
    std::cout << std::fixed << std::setprecision(2);

    uint64_t total = stats.requests.load();
    uint64_t successful = stats.successful.load();
    uint64_t failed = stats.failed.load();
    uint64_t connect_errors = stats.connect_errors.load();

    double success_rate = total > 0 ? (100.0 * successful / total) : 0;
    double fail_rate = total > 0 ? (100.0 * failed / total) : 0;
    double throughput = total / elapsed_seconds;

    std::cout << "Duration:        " << elapsed_seconds << "s\n";
    std::cout << "Connections:     " << config.connections << "\n";
    std::cout << "Total requests:  " << total << "\n";
    std::cout << "Successful:      " << successful << " (" << success_rate << "%)\n";
    std::cout << "Failed:          " << failed << " (" << fail_rate << "%)\n";
    std::cout << "Throughput:      " << static_cast<int>(throughput) << " req/s\n";

    std::vector<uint64_t>& latencies = stats.latencies_us;
    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());

        auto percentile = [&](double p) -> double {
            auto idx = static_cast<size_t>(p * latencies.size() / 100.0);
            idx = std::min(idx, latencies.size() - 1);
            return latencies[idx] / 1000.0;
        };

        double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
        double avg = (sum / latencies.size()) / 1000.0;

        std::cout << "\nLatency (ms):\n";
        std::cout << "  avg:    " << avg << "\n";
        std::cout << "  p50:    " << percentile(50) << "\n";
        std::cout << "  p95:    " << percentile(95) << "\n";
        std::cout << "  p99:    " << percentile(99) << "\n";
        std::cout << "  max:    " << latencies.back() / 1000.0 << "\n";
    }

    std::cout << "\nConnect errors:  " << connect_errors << "\n";
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <host> <port> [options]\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  -c, --connections N   Concurrent connections (default: 100)\n";
    std::cerr << "  -d, --duration SECS   Test duration in seconds (default: 30)\n";
    std::cerr << "  --http                Send HTTP requests (default)\n";
    std::cerr << "  --tcp                 Send raw TCP data\n";
    std::cerr << "\nExample:\n";
    std::cerr << "  " << prog << " 127.0.0.1 8080 -c 50 -d 10 --http\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    Config config;
    config.host = argv[1];
    config.port = std::stoi(argv[2]);
    config.connections = 100;
    config.duration_seconds = 30;
    config.http_mode = true;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--connections") && i + 1 < argc) {
            config.connections = std::stoi(argv[++i]);
        } else if ((arg == "-d" || arg == "--duration") && i + 1 < argc) {
            config.duration_seconds = std::stoi(argv[++i]);
        } else if (arg == "--http") {
            config.http_mode = true;
        } else if (arg == "--tcp") {
            config.http_mode = false;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::cout << "Load generator starting...\n";
    std::cout << "Target:      " << config.host << ":" << config.port << "\n";
    std::cout << "Connections: " << config.connections << "\n";
    std::cout << "Duration:    " << config.duration_seconds << "s\n";
    std::cout << "Mode:        " << (config.http_mode ? "HTTP" : "TCP") << "\n\n";

    Stats stats;
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;

    auto start_time = std::chrono::high_resolution_clock::now();

    workers.reserve(config.connections);
    for (int i = 0; i < config.connections; i++) {
        workers.emplace_back(worker, std::ref(config), std::ref(stats), std::ref(running), i);
    }

    for (int i = 0; i < config.duration_seconds && running.load(); i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "\r[" << (i + 1) << "/" << config.duration_seconds << "s] "
                  << stats.requests.load() << " requests, " << stats.successful.load()
                  << " successful" << std::flush;
    }
    std::cout << "\n";

    running.store(false);

    for (auto& t : workers) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() /
        1000.0;

    print_results(config, stats, elapsed);

    return 0;
}

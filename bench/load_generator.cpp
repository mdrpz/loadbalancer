#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// Simple load generator for benchmarking
// TODO: Implement full load generator (Phase 4)

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port> [connections] [duration_seconds]\n";
        return 1;
    }

    const char* host = argv[1];
    int port = std::stoi(argv[2]);
    int num_connections = (argc > 3) ? std::stoi(argv[3]) : 1000;
    int duration = (argc > 4) ? std::stoi(argv[4]) : 60;

    std::cout << "Load generator: " << num_connections << " connections for " << duration
              << " seconds\n";

    // TODO: Implement connection spawning and data sending

    return 0;
}

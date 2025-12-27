# High-End C++ Load Balancer

A production-grade TCP load balancer built in C++20 using epoll for high-performance I/O.

## Features

- **High Performance**: Single-reactor epoll-based architecture
- **Health Checking**: Automatic backend health monitoring
- **Backpressure Control**: Bounded buffers with overload protection
- **Hot Reload**: Configuration reload without service interruption
- **TLS Termination**: OpenSSL-based TLS support
- **Metrics**: Prometheus-compatible metrics endpoint
- **Zero-Copy**: Optional splice() fast-path for maximum throughput

## Building

### Prerequisites

- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 12+)
- OpenSSL development libraries
- Linux (or WSL)

### Build Steps

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### Build Options

- `BUILD_TESTS=ON` (default): Build C++ unit tests
- `BUILD_BENCH=OFF` (default): Build load generator

```bash
cmake -DBUILD_BENCH=ON ..
```

## Configuration

```yaml
listener:
  host: "0.0.0.0"
  port: 8080

backends:
  - host: "10.0.0.1"
    port: 8000
  - host: "10.0.0.2"
    port: 8000

routing:
  algorithm: "round_robin"
  max_connections_per_backend: 100
  max_global_connections: 1000
```

## Running

```bash
./lb /path/to/config.yaml
```

## Testing

### C++ Unit Tests

```bash
cd build
ctest
```

### Python Integration Tests

```bash
cd tests/python
pytest
```

## Implementation Phases

1. **Phase 1**: Core forwarder (MVP)
2. **Phase 2**: Stability layer (health checks, backpressure)
3. **Phase 3**: Operability (config, metrics, logging)
4. **Phase 4**: Enhancements (TLS, zero-copy, polish)


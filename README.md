# High-End C++ Load Balancer

A production-grade TCP load balancer built in C++20 using epoll for high-performance I/O.

## Features

- **High Performance**: Single-reactor epoll-based architecture
- **Health Checking**: Automatic backend health monitoring with configurable thresholds
- **Backpressure Control**: Bounded buffers with overload protection and timeouts
- **Hot Reload**: Configuration reload without service interruption
- **Metrics**: Prometheus-compatible metrics endpoint
- **Connection Limits**: Global and per-backend connection limits
- **Failure Handling**: Automatic retry with backend failover

## Building

### Prerequisites

- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 12+)
- OpenSSL development libraries
- Linux (or WSL)
- yaml-cpp (optional, for config file support)

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

## Configuration

Configuration can be provided via YAML file or command-line arguments.

### YAML Config File (Recommended)

Create a `config.yaml` file:

```yaml
listener:
  host: "0.0.0.0"
  port: 8080

backends:
  - host: "127.0.0.1"
    port: 8000
  - host: "127.0.0.1"
    port: 8001

routing:
  algorithm: "round_robin"
  max_connections_per_backend: 100
  max_global_connections: 1000

health_check:
  interval_ms: 5000
  timeout_ms: 500
  failure_threshold: 3
  success_threshold: 2

backpressure:
  timeout_ms: 10000

metrics:
  enabled: true
  port: 9090
```

### Command-Line Arguments (Legacy)

```bash
./lb [port] [host] [backend1:port1,backend2:port2,...]
```

Examples:
- `./lb 8080 0.0.0.0 127.0.0.1:8000`
- `./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001`

## Running

### Using Config File

```bash
./lb config.yaml
```

The config file is automatically reloaded every 5 seconds if modified. Changes to backends will mark removed backends as DRAINING (existing connections continue, no new connections routed).

### Using Command-Line

```bash
./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001
```

## Testing

### Quick Test

1. Start a backend server:
   ```bash
   cd tests/python
   python3 test_backend.py 8000
   ```

2. Start the load balancer:
   ```bash
   cd build
   ./lb config.yaml
   # or: ./lb 8080 0.0.0.0 127.0.0.1:8000
   ```

3. Test the connection:
   ```bash
   nc localhost 8080
   # Type messages - they should be echoed back
   ```

### Advanced Testing

**Health Checker**: Stop/start backends to see health state transitions (checks run every 5 seconds)

**Connection Limits**: Connect more than 100 clients to a single backend or 1000 total to test limits

**Backpressure**: Send data faster than backend can consume; slow backends (>10s) timeout

**Failure Handling**: Start with non-existent backends to see retry logic (up to 3 attempts)

**Config Hot Reload**: Edit `config.yaml` while running to see backends marked as DRAINING when removed

### Metrics

Access Prometheus metrics at `http://localhost:9090/metrics`:

- `lb_connections_total`: Total connections accepted
- `lb_connections_active`: Current active connections
- `lb_backend_routed_total{backend="..."}`: Connections routed per backend
- `lb_backend_failures_total{backend="..."}`: Backend failures
- `lb_backend_routes_failed_total`: Failed route attempts
- `lb_overload_drops`: Connections dropped due to overload

## Development

### Running Tests

```bash
cd build
ctest                    # C++ unit tests
cd ../tests/python
pytest                   # Python integration tests
```

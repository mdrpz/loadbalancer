# C++ Load Balancer

A TCP load balancer built in C++20 using epoll for high-performance I/O

## Features

- **High Performance**: Single-reactor epoll-based architecture
- **TLS/SSL Termination**: Encrypt client connections while backends remain plain TCP
- **Health Checking**: Automatic backend health monitoring with configurable thresholds
- **Backpressure Control**: Bounded buffers with overload protection and timeouts
- **Hot Reload**: Configuration reload without service interruption
- **Metrics**: Prometheus-compatible metrics endpoint
- **Connection Limits**: Global and per-backend connection limits
- **Failure Handling**: Automatic retry with backend failover

## Building

### Prerequisites

- CMake 3.20+, C++20 compiler (GCC 10+, Clang 12+)
- OpenSSL development libraries
- yaml-cpp: `sudo apt-get install libyaml-cpp-dev` (Debian/Ubuntu) or `sudo dnf install yaml-cpp-devel` (Fedora/RHEL)
- Linux (or WSL)

### Build Steps

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

**Note:** If yaml-cpp is installed after running `cmake ..`, reconfigure with `cmake ..` again

### Build Options

- `BUILD_TESTS=ON` (default): Build C++ unit tests
- `BUILD_BENCH=OFF` (default): Build load generator

## Configuration

### YAML Config File (Recommended)

Edit `config.yaml` in the project root. The config file is automatically reloaded every 5 seconds if modified

**TLS Configuration:**

To enable TLS termination, set `tls_enabled: true` in `config.yaml` and generate certificates:
```bash
./scripts/generate_test_cert.sh
```

**Note:** TLS is terminated at the load balancer; backends receive plain TCP connections

### Command-Line Arguments (Legacy)

```bash
./lb [port] [host] [backend1:port1,backend2:port2,...]
# Example: ./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001
```

## Running

### Using Config File

```bash
cd build
./lb ../config.yaml
```

Config changes are automatically reloaded every 5 seconds. Removed backends are marked as DRAINING (existing connections continue, no new connections routed)

### Using Command-Line

```bash
./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001
```

## Testing

### Quick Test (TCP mode)

1. Start backend: `cd tests/python && python3 test_backend.py 8000`
2. Start load balancer: `cd build && ./lb ../config.yaml`
3. Test: `nc localhost 8080` (type messages - they should be echoed back)

### Advanced Testing

- **Health Checker**: Stop/start backends to see health transitions (checks every 5s)
- **Connection Limits**: Connect >100 clients per backend or >1000 total
- **Backpressure**: Send data faster than backend consumes; slow backends (>10s) timeout
- **Failure Handling**: Start with non-existent backends to see retry logic (up to 3 attempts)
- **Config Hot Reload**: Edit `config.yaml` while running to see DRAINING state
- **TLS Testing**: Enable TLS and test with `openssl s_client -connect localhost:8080` or `curl -k https://localhost:8080`

### HTTP Mode + Python Integration Tests

- HTTP mode is enabled via config: set `listener.mode: "http"` in `config.yaml`.
- Python integration tests live under `tests/python` and spin up real TCP backends plus the `lb` binary.

Run the Python tests:

```bash
cd build
cmake --build .
cd ../tests/python
python3 -m pip install -r requirements.txt
python3 -m pytest -v
```

The routing suite covers:

- Round-robin distribution and alternation across backends
- Least-connections routing
- Health checking (unhealthy backends stop receiving traffic)
- Sequential and concurrent request handling

### Metrics

Access Prometheus metrics at `http://localhost:9090/metrics`:

- `lb_connections_total`: Total connections accepted
- `lb_connections_active`: Current active connections
- `lb_backend_routed_total{backend="..."}`: Connections routed per backend
- `lb_backend_failures_total{backend="..."}`: Backend failures
- `lb_backend_routes_failed_total`: Failed route attempts
- `lb_overload_drops`: Connections dropped due to overload

## Development

### Code Formatting and Linting

Install: `sudo apt-get install clang-format clang-tidy` (Debian/Ubuntu) or `sudo dnf install clang-tools-extra` (Fedora/RHEL)

```bash
# Format
find src tests bench -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Lint
find src tests bench -name "*.cpp" -o -name "*.h" | xargs clang-tidy -p build

# Auto-fix
find src tests bench -name "*.cpp" -o -name "*.h" | xargs clang-tidy -p build --fix
```

Configuration: `.clang-format` (LLVM-based), `.clang-tidy` (static analysis)

#### Python (Ruff)

Python formatting and linting is handled by Ruff:

```bash
python3 -m pip install --user ruff

# Lint
python3 -m ruff check tests/python

# Auto-fix and format
python3 -m ruff check tests/python --fix
python3 -m ruff format tests/python
```

### Running Tests

```bash
cd build
ctest
cd ../tests/python
pytest
```

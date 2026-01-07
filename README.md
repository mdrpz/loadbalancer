# C++ Load Balancer

High-performance TCP/HTTP load balancer using C++20 and epoll.

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install cmake g++ libssl-dev libyaml-cpp-dev

# Build
mkdir build && cd build
cmake .. && cmake --build .

# Start a backend
python3 -m http.server 8000 &

# Run load balancer
./lb ../config.yaml

# Test
curl http://localhost:8080/
```

## Configuration

Edit `config.yaml`:

```yaml
listener:
  host: "0.0.0.0"
  port: 8080
  mode: "http"              # "tcp" or "http"
  tls_enabled: false

backends:
  - host: "127.0.0.1"
    port: 8000

routing:
  algorithm: "round_robin"  # or "least_connections"
```

Changes auto-reload every 5 seconds.

## Running Tests

### Python Integration Tests

```bash
cd tests/python
pip install -r requirements.txt
python3 -m pytest -v
```

**Test coverage:** Round-robin, least-connections, health checks, connection handling, config hot reload, graceful shutdown, TLS termination

### C++ Unit Tests

```bash
cd build && ctest
```

## Benchmarking

```bash
# Build with load generator
cd build
cmake .. -DBUILD_BENCH=ON && cmake --build .

# Run (against running LB on port 8080)
./bench/load_generator 127.0.0.1 8080 -c 100 -d 30 --http
```

## Development

### Build Commands

```bash
cmake .. -DBUILD_BENCH=ON    # Include load generator
cmake .. -DBUILD_TESTS=ON    # Include C++ tests (default: ON)
cmake --build . -j$(nproc)   # Parallel build
```

### Linting & Formatting

```bash
# C++
find src bench -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Python
python3 -m ruff check . --fix && python3 -m ruff format .
```

### CI

GitHub Actions runs on push/PR to `main`:
- Build (with tests + bench)
- C++ tests (`ctest`)
- Python tests (`pytest`)
- Format/lint checks (`clang-format`, `ruff`)

### Rebuild After Changes

```bash
cd build && cmake --build .
```

## Architecture

```
                    +----------------+
                    | Health Checker |
                    +-------+--------+
                            |
                            v
+---------+       +-------------------+       +-----------+
| Clients | ----> |   Load Balancer   | ----> | Backend 1 |
+---------+       |                   |       +-----------+
                  |  - TLS terminate  |       +-----------+
                  |  - Route (RR/LC)  | ----> | Backend 2 |
                  |  - HTTP headers   |       +-----------+
                  +-------------------+       +-----------+
                            |           ----> | Backend N |
                            v                 +-----------+
                    +----------------+
                    | Metrics :9090  |
                    +----------------+
```

**Request flow:**

1. Client connects to LB (port 8080)
2. LB performs TLS handshake if `tls_enabled: true`
3. LB selects a healthy backend using configured algorithm
4. LB connects to backend (plain TCP, even if client used TLS)
5. Data flows bidirectionally between client ↔ LB ↔ backend
6. In HTTP mode, LB injects `X-Forwarded-*` headers before forwarding

**Key components:**

- **Epoll Reactor** – Single-threaded event loop handling all I/O
- **Backend Pool** – Manages backend list and routing algorithm
- **Health Checker** – Background thread pinging backends periodically
- **Connection Manager** – Tracks client↔backend pairs, handles cleanup

## Features

HTTP Mode: Adds `X-Forwarded-For`, `X-Real-IP`, `X-Forwarded-Proto` headers

TLS: Terminates TLS; backends receive plain TCP

Health Checks: Auto-removes unhealthy backends

Hot Reload: Config changes apply without restart

Metrics: Prometheus endpoint at `:9090/metrics`

Routing: Round-robin or least-connections

## TLS Setup

```bash
./scripts/generate_test_cert.sh
```

Then set `tls_enabled: true` in config.yaml.

## Metrics

Access at `http://localhost:9090/metrics`:

- `lb_connections_total` – Total connections
- `lb_connections_active` – Active connections
- `lb_backend_routed_total{backend="..."}` – Per-backend routing
- `lb_backend_failures_total{backend="..."}` – Per-backend failures
- `lb_overload_drops` – Dropped due to limits

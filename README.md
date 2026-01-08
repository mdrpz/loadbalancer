# C++ Load Balancer

High-performance TCP/HTTP load balancer using C++20 and epoll.

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install cmake g++ libssl-dev libyaml-cpp-dev

# Build
mkdir build && cd build
cmake .. && cmake --build .

# Start a backend & run
python3 -m http.server 8000 &
./lb ../config.yaml

# Test
curl http://localhost:8080/
```

## Configuration

Edit `config.yaml` (auto-reloads every 5 seconds):

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

connection_pool:
  enabled: true             # Reuse backend connections
  max_connections: 10       # Per backend
```

## Features

- **HTTP Mode** - Adds `X-Forwarded-For`, `X-Real-IP`, `X-Forwarded-Proto` headers
- **TLS Termination** - Backends receive plain TCP
- **Health Checks** - Auto-removes unhealthy backends
- **Hot Reload** - Config changes apply without restart
- **Connection Pooling** - Reuses backend connections (-36% p95 latency)
- **Routing** - Round-robin or least-connections
- **Metrics** - Prometheus endpoint at `:9090/metrics`

## Architecture

```
                    +----------------+
                    | Health Checker |
                    +-------+--------+
                            |
+---------+       +-------------------+       +-----------+
| Clients | ----> |   Load Balancer   | ----> | Backends  |
+---------+       | - TLS terminate   |       +-----------+
                  | - Route (RR/LC)   |
                  | - Connection pool |
                  +-------------------+
                            |
                    +----------------+
                    | Metrics :9090  |
                    +----------------+
```

## Tests & Benchmarks

```bash
# Python integration tests
cd tests/python && pip install -r requirements.txt
python3 -m pytest -v

# C++ unit tests
cd build && ctest

# Load generator
cmake .. -DBUILD_BENCH=ON && cmake --build .
./bench/load_generator 127.0.0.1 8080 -c 100 -d 10
```

## TLS Setup

```bash
./scripts/generate_test_cert.sh
# Then set tls_enabled: true in config.yaml
```

## Zero-Copy Mode (Experimental)

> Has issues with concurrent connections. Use `use_splice: false` for production.

```yaml
listener:
  mode: "tcp"
  use_splice: true  # Linux only, TCP mode only
```

## Metrics

Available at `http://localhost:9090/metrics`:

- `lb_connections_total` / `lb_connections_active`
- `lb_backend_routed_total{backend="..."}`
- `lb_backend_failures_total{backend="..."}`
- `lb_overload_drops`

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

## Backend Setup

### Python HTTP Server (Quick Testing)

Best for: Quick testing, development, simple HTTP responses

```bash
# Start Python backend
python3 -m http.server 8000

# Configure in config.yaml
backends:
  - host: "127.0.0.1"
    port: 8000
```

### Nginx Backend (Production Testing)

Best for: Production-like testing, performance benchmarks, realistic HTTP behavior

```bash
# Install nginx (Ubuntu/Debian)
sudo apt install nginx

# Enable and start
sudo systemctl restart nginx

# Configure in config.yaml
backends:
  - host: "127.0.0.1"
    port: 80
```

### Load Testing

**Important:** The load generator mode must be compatible with the listener mode in `config.yaml`:
- **LB in TCP mode** (`mode: "tcp"`): Use `--tcp` flag. Sends raw TCP bytes; no HTTP parsing/metrics/access logs.
- **LB in HTTP mode** (`mode: "http"`): Use default (HTTP) or `--http` flag. Sends HTTP requests; full HTTP parsing/metrics/access logs.

```bash
# HTTP mode - config.yaml must have mode: "http"
./bench/load_generator 127.0.0.1 8080 -c 100 -d 10

# TCP mode - use --tcp flag and config.yaml must have mode: "tcp"
./bench/load_generator 127.0.0.1 8080 -c 100 -d 10 --tcp

# Test with Nginx backend (higher concurrency, HTTP mode)
./bench/load_generator 127.0.0.1 8080 -c 500 -d 30

# Test with custom parameters (TCP mode)
./bench/load_generator 127.0.0.1 8080 -c 200 -d 60 --tcp  # 200 connections, 60 seconds
```

## Docker

```bash
# Run with docker-compose (includes 2 nginx backends)
docker-compose up --build

# Test
curl http://localhost:8080/

# Or build image only
docker build -t loadbalancer .
docker run -p 8080:8080 -p 9090:9090 -v $(pwd)/config.yaml:/app/config.yaml loadbalancer
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
    port: 8000              # or port 80 if using nginx backend
    weight: 3               # Gets 3x traffic (default: 1)
  - host: "127.0.0.1"
    port: 8001
    weight: 1               # Gets 1x traffic

routing:
  algorithm: "round_robin"  # or "least_connections"

connection_pool:
  enabled: true             # Reuse backend connections
  max_connections: 10       # Per backend

logging:
  access_log_enabled: true
  access_log_file: "/var/log/lb_access.log"

timeouts:
  request_ms: 30000 
```

## Features

- **HTTP Mode** - Adds `X-Forwarded-For`, `X-Real-IP`, `X-Forwarded-Proto` headers
- **TLS Termination** - Backends receive plain TCP
- **Health Checks** - Auto-removes unhealthy backends
- **Hot Reload** - Config changes apply without restart
- **Connection Pooling** - Reuses backend connections (-36% p95 latency)
- **Routing** - Round-robin, least-connections, weighted
- **Metrics** - Prometheus endpoint at `:9090/metrics`
- **Access Logging** - Combined log format (like nginx) with per-request logging
- **Request Timeouts** - Automatic connection timeout for slow clients with per-request logging
- **IP Filtering** - Whitelist/blacklist IP addresses for security
- **HTTP Error Responses** - Proper HTTP error responses when rejecting connections

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

# Build load generator
cmake .. -DBUILD_BENCH=ON && cmake --build .

# Run load tests (see Backend Setup section for more examples)
# HTTP mode (default)
./bench/load_generator 127.0.0.1 8080 -c 100 -d 10

# TCP mode (use --tcp flag, ensure config.yaml has mode: "tcp")
./bench/load_generator 127.0.0.1 8080 -c 100 -d 10 --tcp
```

### Performance

Example benchmark results with nginx (100 concurrent connections, 10 seconds):

**HTTP Mode:**
```
Duration:        10.04s
Connections:     100
Total requests:  29400
Successful:      29400 (100.00%)
Failed:          0 (0.00%)
Throughput:      2927 req/s

Latency (ms):
  avg:    34.07
  p50:    32.40
  p95:    46.86
  p99:    53.30
  max:    93.60
```

**TCP Mode:**
```
Duration:        10.02s
Connections:     100
Total requests:  32200
Successful:      32200 (100.00%)
Failed:          0 (0.00%)
Throughput:      3212 req/s

Latency (ms):
  avg:    31.07
  p50:    29.39
  p95:    43.44
  p99:    52.33
  max:    63.24
```

## Code Formatting & Linting

```bash
# Format C++ code
find src bench -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Check C++ formatting
find src bench -name "*.cpp" -o -name "*.h" | xargs clang-format --dry-run --Werror

# Format Python code
python3 -m ruff format .

# Lint Python code
python3 -m ruff format check --fix .
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

## Access Logging

Access logs are written in combined log format (similar to nginx/Apache). Each HTTP request is logged with client IP, method, path, status code, bytes transferred, latency, and backend server.

Logs are written asynchronously to avoid blocking the event loop. Configure in `config.yaml`:

```yaml
logging:
  access_log_enabled: true
  access_log_file: "/tmp/lb_access.log"
```

## Request Timeouts

The load balancer can automatically close connections that exceed a configured timeout. This prevents slow clients from holding connections open indefinitely.

Configure in `config.yaml`:

```yaml
timeouts:
  request_ms: 5000  # Close connections after 5 seconds
```

Timed-out requests are tracked in the `lb_request_timeouts_total` metric.

## IP Filtering

The load balancer supports IP whitelisting and blacklisting for security. Configure in `config.yaml`:

```yaml
ip_filter:
  whitelist: []  # Empty = disabled. If non-empty, only these IPs are allowed
  blacklist: []  # Empty = disabled. These IPs are always rejected
```

**Behavior:**
- **Blacklist**: Always checked first. If an IP is blacklisted, connection is rejected.
- **Whitelist**: If non-empty, only IPs in the whitelist are allowed. If empty, all IPs (except blacklisted) are allowed.
- **HTTP Mode**: Rejected connections receive proper HTTP error responses (403 Forbidden for IP filtering, 503 Service Unavailable for connection limits).
- **TCP Mode**: Rejected connections are simply closed.

**Examples:**
```yaml
# Only allow localhost
ip_filter:
  whitelist: ["127.0.0.1"]
  blacklist: []

# Block specific IPs
ip_filter:
  whitelist: []
  blacklist: ["10.0.0.1", "192.168.1.100"]
```

## Metrics

Prometheus metrics are served at http://localhost:9090/metrics

The following metrics are available:

`lb_connections_total` (counter): Total number of connections accepted.

`lb_connections_active` (gauge): Current number of active connections.

`lb_bytes_received_total` (counter): Total bytes received from clients.

`lb_bytes_sent_total` (counter): Total bytes sent to clients.

`lb_request_duration_ms_bucket` (histogram): Distribution of request latencies.

`lb_backend_requests_total{backend="..."}` (counter): Number of requests per backend.

`lb_backend_errors_total{backend="..."}` (counter): Number of errors per backend.

`lb_request_timeouts_total` (counter): Number of timed out requests.

`lb_overload_drops_total` (counter): Number of requests dropped due to overload.

## Troubleshooting

**Port already in use:**
```bash
# Find what's using a port
lsof -i :8080

# Kill process on a specific port
kill $(lsof -t -i :8080)

# Or force kill
kill -9 $(lsof -t -i :8080)
```

**Kill all test servers:**
```bash
pkill -f "http.server"
pkill -f "./lb"
```

**Check if LB is running:**
```bash
curl -s http://localhost:8080/ && echo "LB OK" || echo "LB not responding"
```

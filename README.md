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

### Echo Server (TCP Mode Testing)

Best for: TCP mode testing, simple echo functionality, debugging

The project includes a simple echo server utility for testing TCP mode:

```bash
# Start echo server on port 8000
python3 tests/python/backend_server.py 8000

# Configure in config.yaml (TCP mode)
listener:
  mode: "tcp"
backends:
  - host: "127.0.0.1"
    port: 8000
```

The echo server will echo back any data sent to it, making it useful for testing raw TCP forwarding.

### Load Testing

**Important:** The load generator mode must be compatible with the listener mode in `config.yaml`:
- **Load generator defaults to HTTP mode** (does not read `config.yaml`). Use `--tcp` flag for TCP mode.
- **LB in TCP mode** (`mode: "tcp"`): Use `--tcp` flag. Sends raw TCP bytes; no HTTP parsing/metrics/access logs.
- **LB in HTTP mode** (`mode: "http"`): Use default (HTTP) or `--http` flag. Sends HTTP requests; full HTTP parsing/metrics/access logs.

```bash
# HTTP mode (default) - config.yaml must have mode: "http"
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
- **Custom HTTP Headers** - Add, modify, or remove HTTP headers in requests and responses
- **Connection Rate Limiting** - Limit connections per IP address to prevent abuse

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
cd build
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
python3 -m ruff check --fix .
```

## TLS Setup

```bash
./scripts/generate_test_cert.sh
# Then set tls_enabled: true in config.yaml
```

## Zero-Copy Mode (Experimental)

The load balancer can use Linux's `splice()` system call for zero-copy data transfer, reducing CPU overhead by avoiding user-space copies.

Configure in `config.yaml`:

```yaml
listener:
  mode: "tcp"
  use_splice: true  # Linux only, TCP mode only
```

**Behavior:**
- **Linux Only**: Requires Linux kernel (uses `splice()` system call)
- **TCP Mode Only**: Only works in TCP mode, not HTTP mode
- **Zero-Copy**: Data is transferred directly between file descriptors without copying to user space
- **Experimental**: Has issues with concurrent connections; use `use_splice: false` for production

## Access Logging

The load balancer logs HTTP requests in combined log format (similar to nginx/Apache). Each request is logged with client IP, method, path, status code, bytes transferred, latency, and backend server.

Configure in `config.yaml`:

```yaml
logging:
  access_log_enabled: true
  access_log_file: "/tmp/lb_access.log"
```

**Behavior:**
- **Combined Format**: Logs follow standard combined log format (client IP, timestamp, method, path, status, bytes, latency, backend)
- **Asynchronous**: Logs are written asynchronously to avoid blocking the event loop
- **Per-Request**: Each HTTP request is logged when its response completes
- **Keep-Alive Support**: Multiple requests on the same connection are logged separately

## Request Timeouts

The load balancer can automatically close connections that exceed a configured timeout. This prevents slow clients from holding connections open indefinitely.

Configure in `config.yaml`:

```yaml
timeouts:
  request_ms: 5000  # Close connections after 5 seconds
```

**Behavior:**
- **Per-Connection**: Timeout is measured from connection establishment
- **Automatic Cleanup**: Connections exceeding the timeout are automatically closed
- **Periodic Checking**: Timeouts are checked every second
- **Metrics**: Timed-out requests are tracked in the `lb_request_timeouts_total` metric

## IP Filtering

The load balancer supports IP whitelisting and blacklisting for security. Configure in `config.yaml`:

```yaml
ip_filter:
  whitelist: []  # Empty = disabled. If non-empty, only these IPs are allowed
  blacklist: []  # Empty = disabled. These IPs are always rejected
```

**Behavior:**
- **Blacklist First**: Blacklist is checked first; blacklisted IPs are always rejected
- **Whitelist**: If non-empty, only whitelisted IPs are allowed (all others rejected)
- **Empty Lists**: Empty lists mean disabled (all IPs allowed except blacklisted)
- **HTTP Mode**: Rejected connections receive HTTP 403 Forbidden response
- **TCP Mode**: Rejected connections are simply closed

## Custom HTTP Headers

The load balancer can add, modify, or remove HTTP headers in both requests (before forwarding to backends) and responses (before sending to clients). This is useful for adding security headers, custom routing headers, or removing sensitive information.

Configure in `config.yaml`:

```yaml
http_headers:
  request:
    add:
      X-Custom-Header: "value"
      X-LB-Version: "1.0"
      X-Security-Token: "abc123"
    remove:
      - "User-Agent"
      - "X-Original-Header"
  response:
    add:
      X-Frame-Options: "DENY"
      X-Content-Type-Options: "nosniff"
      X-XSS-Protection: "1; mode=block"
    remove:
      - "Server"
```

**Behavior:**
- **Request Headers**: Modified before forwarding to backends
- **Response Headers**: Modified before sending to clients
- **Add/Overwrite**: Headers are added or overwritten if they already exist (case-insensitive matching)
- **Remove First**: Headers are removed first, then added/modified
- **Case-Insensitive**: Header name matching is case-insensitive (e.g., "user-agent" matches "User-Agent")
- **Hot Reload**: Header modifications update on config reload without restart

## Connection Rate Limiting

The load balancer can limit the number of connections per IP address within a time window. This helps prevent abuse and DDoS attacks by limiting how many connections a single IP can establish.

Configure in `config.yaml`:

```yaml
rate_limit:
  enabled: true
  max_connections: 100  # Max connections per IP
  window_seconds: 60     # Time window in seconds
```

**Behavior:**
- **Sliding Window**: Counts connections within the last N seconds (sliding window)
- **Per-IP**: Each IP address is tracked separately
- **Automatic Cleanup**: Old connection timestamps are automatically removed
- **HTTP Mode**: Rejected connections receive HTTP 429 Too Many Requests response
- **TCP Mode**: Rejected connections are simply closed

## Request Queuing

The load balancer can queue incoming connections when the global connection limit is reached, instead of immediately rejecting them. This helps smooth out short bursts of traffic.

Configure in `config.yaml`:

```yaml
queue:
  enabled: true
  max_queue_size: 1024
  max_wait_ms: 5000
```

**Behavior:**
- **Global Limit**: Queues connections when `max_global_connections` is reached
- **FIFO Queue**: Connections are served in the order they were queued
- **Max Queue Size**: New connections are dropped when the queue is full
- **Queue Timeout**: Queued connections are dropped if they wait longer than `max_wait_ms`
- **HTTP Mode**: Dropped queued connections receive HTTP 503 Service Unavailable
- **TCP Mode**: Dropped queued connections are simply closed

## Sticky Sessions / Session Affinity

The load balancer can route the same client to the same backend server, ensuring session persistence for stateful applications.

Configure in `config.yaml`:

```yaml
routing:
  sticky_sessions:
    enabled: true
    method: "cookie"  # "cookie" or "ip"
    cookie_name: "LB_SESSION"
    ttl_seconds: 3600
```

**Behavior:**
- **Cookie-Based**: Uses HTTP cookies to track sessions (HTTP mode only)
  - Extracts session cookie from incoming requests
  - Injects `Set-Cookie` header in responses if cookie is missing
  - Routes to the same backend as long as the cookie is valid
- **IP-Based**: Uses client IP address to track sessions (works for both HTTP and TCP)
  - Routes clients from the same IP to the same backend
  - Simpler but less reliable behind proxies/NAT
- **Session TTL**: Sessions expire after `ttl_seconds` of inactivity
- **Automatic Cleanup**: Expired sessions are automatically removed
- **Fallback**: If sticky backend is unhealthy or at capacity, falls back to normal routing algorithm
- **Hot Reload**: Sticky session settings update on config reload without restart

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

`lb_rate_limit_drops_total` (counter): Number of connections dropped due to rate limiting.

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

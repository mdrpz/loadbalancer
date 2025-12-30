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

The load balancer currently accepts backends via command-line arguments.

### Basic Usage

```bash
./lb [port] [host] [backend1:port1,backend2:port2,...]
```

### Examples

**Single backend:**
```bash
./lb 8080 0.0.0.0 127.0.0.1:8000
```

**Multiple backends:**
```bash
./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001,127.0.0.1:8002
```

**Custom listen address:**
```bash
./lb 8080 127.0.0.1 127.0.0.1:8000
```

### Arguments

- `port`: Port to listen on (default: 8080)
- `host`: Host address to bind to (default: 0.0.0.0)
- `backends`: Comma-separated list of backend addresses in `host:port` format

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

### Manual Testing

You can manually test the load balancer with a simple echo backend server.

#### Step 1: Start a Backend Server

In one terminal, start the test backend server:

```bash
cd tests/python
python3 test_backend.py 8000
```

You should see:
```
Backend server listening on 127.0.0.1:8000
```

#### Step 2: Start the Load Balancer

In another terminal, build and start the load balancer:

```bash
cd build
cmake --build .
./lb 8080 0.0.0.0 127.0.0.1:8000
```

This starts the load balancer on port 8080, forwarding to the backend on 127.0.0.1:8000.

You should see:
```
Load Balancer listening on 0.0.0.0:8080
Press Ctrl+C to stop...
```

#### Step 3: Test the Load Balancer

In a third terminal, test the connection:

**Option A: Interactive test (recommended)**
```bash
nc localhost 8080
# Type messages and press Enter
# You should see them echoed back
```

**Option B: Single message test**
```bash
echo "Hello!" | nc localhost 8080
```

**Option C: Multiple messages**
```bash
(echo "message1"; sleep 1; echo "message2") | nc localhost 8080
```

#### Expected Behavior

- Messages sent to the load balancer (port 8080) should be forwarded to the backend (port 8000)
- The backend echoes messages back
- Responses from the backend should be forwarded back to the client
- You should see connection logs in the backend terminal

**Note:** When using `echo "message" | nc`, the client closes immediately after sending, which may cause the backend to timeout waiting for more data. This is expected behavior. For interactive testing, use `nc localhost 8080` without piping.

#### Testing with Multiple Backends

To test with multiple backends, start additional backend servers on different ports:

```bash
# Terminal 1
python3 test_backend.py 8000

# Terminal 2
python3 test_backend.py 8001

# Terminal 3 - Start load balancer with multiple backends
./lb 8080 0.0.0.0 127.0.0.1:8000,127.0.0.1:8001
```

The load balancer will distribute connections across the available backends.

#### Advanced Testing

**Health Checker:**
- Start a backend, then stop it - the health checker should mark it as UNHEALTHY
- Restart the backend - it should be marked HEALTHY again after 2 successful checks
- Health checks run every 5 seconds by default

**Connection Limits:**
- Try connecting more than 100 clients to a single backend - excess connections should be rejected
- Try connecting more than 1000 total clients - excess should be rejected globally

**Backpressure:**
- Send data faster than the backend can consume - the load balancer should handle it gracefully
- If a backend is too slow (>10 seconds), the connection should timeout and close

**Failure Handling & Retry:**
- Start load balancer with a non-existent backend - it should retry up to 3 times
- Mix healthy and unhealthy backends - should retry and eventually connect to healthy one
- Stop a backend during connection - should retry with next backend

## Implementation Phases

1. **Phase 1**: Core forwarder (MVP) X
2. **Phase 2**: Stability layer (health checks, backpressure, connection limits, failure handling) X
3. **Phase 3**: Operability (config, metrics, logging)
4. **Phase 4**: Enhancements (TLS, zero-copy, polish)

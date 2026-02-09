# Architecture & Internals


## 1. System Architecture

Clients connect to a **TcpListener** that hands accepted fds to the **LoadBalancer** orchestrator. The **EpollReactor** drives all I/O in a single event loop and fires a periodic maintenance tick. **EventHandlers** dispatch read/write/error events per fd. The **BackendPool** selects a healthy backend (round-robin or least-connections), and the **BackendConnector** opens (or borrows) a backend socket. Data flows through **DataForwarder** (TCP), **HttpDataForwarder** (HTTP with header injection), or **SpliceForwarder** (zero-copy). A **HealthChecker** runs on its own thread and updates backend state via atomics. The **ThreadPool** runs background work: log flushing, config reload, and the **MetricsServer** accept loop.

```
  Clients --> TcpListener --> LoadBalancer (orchestrator)
                                  |
                    +-------------+-------------+
                    |                           |
              EpollReactor                EventHandlers
              (event loop)                (I/O dispatch)
                    |                           |
              periodic tick (1s)                |
                    |                           v
              Maintenance               Connection (fd + bufs)
              (timeouts, config, GC)
                                         BackendPool (RR / LC)
                                              |
  HealthChecker (own thread) --------> BackendNode (state, weight)

  ThreadPool (N workers) <-- Logger / AccessLogger
                             MetricsServer (:9090)
                             TlsContext (OpenSSL)
```

### Namespaces

- **`lb::net`** — Networking primitives: epoll reactor, TCP listener, connection abstraction.
- **`lb::core`** — Load balancer orchestration: routing, backend management, forwarding, backpressure, thread pool.
- **`lb::config`** — YAML configuration parsing, hot-reload watcher.
- **`lb::health`** — Periodic TCP/HTTP health probes.
- **`lb::metrics`** — Atomic counters, Prometheus export, metrics HTTP server.
- **`lb::tls`** — OpenSSL context, per-connection SSL wrapping.
- **`lb::logging`** — Async logger and access logger (combined log format).
- **`lb::http`** — HTTP request/response parsing and serialisation.

### Core Components

**`EpollReactor`** (`net/epoll_reactor.h`)
Single-threaded, level-triggered epoll event loop. Dispatches READ/WRITE/ERROR/HUP per fd. Fires a periodic callback every N ms for maintenance tasks.

**`TcpListener`** (`net/tcp_listener.h`)
RAII socket bind + listen + non-blocking accept.

**`Connection`** (`net/connection.h`)
Owns an fd plus a 64 KB read buffer and 64 KB write buffer. Tracks bytes read/written, memory-budget accounting, and optional SSL pointer. States: HANDSHAKE, CONNECTING, ESTABLISHED, CLOSED.

**`LoadBalancer`** (`core/load_balancer.h`)
Top-level orchestrator. Owns the reactor, listener, backend pool, health checker, TLS context, thread pool, and all connection state maps. Implements `handle_accept()`, periodic maintenance, and config application.

**`BackendPool`** (`core/backend_pool.h`)
Holds a list of `BackendNode`s. Selects the next backend via weighted round-robin or least-connections. Skips unhealthy/draining nodes.

**`BackendNode`** (`core/backend_node.h`)
Per-backend state: host, port, weight, atomic BackendState (HEALTHY / UNHEALTHY / DRAINING), atomic active-connection count, atomic failure count.

**`ConnectionManager`** (`core/connection_manager.h`)
Centralises connection bookkeeping: close, cleanup, release to pool, access-log callback.

**`EventHandlers`** (`core/event_handlers.h`)
Receives (fd, EventType) from the reactor and dispatches to the correct handler (client read/write, backend read/write, TLS handshake, backpressure, retry).

**`BackendConnector`** (`core/backend_connector.h`)
Opens (or borrows from pool) a backend socket, pairs it with the client connection, registers both with the reactor.

**`DataForwarder`** (`core/data_forwarder.h`)
Copies bytes between paired connections (TCP mode). Triggers backpressure when the write buffer fills.

**`HttpDataForwarder`** (`core/http_data_forwarder.h`)
Parses HTTP request/response, injects/removes headers, records RequestInfo for access logging, forwards modified payloads.

**`SpliceForwarder`** (`core/splice_forwarder.h`)
Experimental zero-copy path using `splice()` with kernel pipe pairs. TCP mode only.

**`ConnectionPoolManager`** (`core/connection_pool.h`)
Per-backend idle-connection pools (borrow / release / evict).

**`BackpressureManager`** (`core/backpressure_manager.h`)
Tracks fds under write-buffer pressure; times out stalled connections.

**`RetryHandler`** (`core/retry_handler.h`)
Re-dispatches a client connection to another backend on failure (up to 3 attempts).

**`SessionManager`** (`core/session_manager.h`)
Cookie-based or IP-based sticky session table with TTL expiry.

**`MemoryBudget`** (`core/memory_budget.h`)
Global lock-free byte budget (CAS-based try_reserve / release). Connections that exceed the budget are rejected.

**`ThreadPool`** (`core/thread_pool.h`)
Fixed-size pool of std::thread workers executing std::function<void()> tasks from a shared FIFO queue.

**`HealthChecker`** (`health/health_checker.h`)
Dedicated thread that periodically probes every backend (TCP connect or HTTP GET). Updates BackendNode state using consecutive success/failure thresholds.

**`TlsContext`** (`tls/tls_context.h`)
Creates and owns an OpenSSL SSL_CTX; creates per-connection SSL objects.

**`ConfigManager`** (`config/config.h`)
Parses YAML config into an immutable Config snapshot. `check_and_reload()` compares file mtime and atomically swaps the config pointer.

**`Logger`** (`logging/logger.h`)
Singleton async logger. Enqueues formatted log lines; drains via internal worker thread or thread pool.

**`AccessLogger`** (`logging/access_logger.h`)
Singleton async access logger (combined format). Same dual-mode drain as Logger.

**`MetricsServer`** (`metrics/metrics_server.h`)
Standalone HTTP listener on its own port. Serves `/metrics` (Prometheus) and `/health` (JSON). Runs its accept loop on the thread pool or a dedicated thread.

**`Metrics`** (`metrics/metrics.h`)
Singleton holding all atomic counters and a latency histogram. `export_prometheus()` serialises to Prometheus text format.


## 2. Threading Model

The system uses **one main event-loop thread** plus a small set of background threads.

### Thread Inventory

**1. Reactor thread (main)**
Created by `main()` via `LoadBalancer::run()` then `EpollReactor::run()`. Runs the epoll event loop. All I/O (accept, read, write, TLS handshake) and the periodic maintenance callback execute here. This is the only thread that touches connection state maps.

**2. Thread pool (N workers, default 4)**
Created by `LoadBalancer::initialize_from_config()`. General-purpose background workers. Used for log flushing (Logger, AccessLogger), config file stat + reload, and MetricsServer accept loop + request handling.

**3. Health checker thread**
Created by `HealthChecker::start()`. Sleeps for `interval_ms`, then sequentially probes every backend. Updates atomic `BackendNode::state()` — the only cross-thread write to backend state.

**4. Shutdown watcher thread**
Created by `main()`. Polls the `g_shutdown` flag every 50 ms; calls `shutdown_gracefully()` when a signal arrives.

> When the thread pool is not configured (`thread_pool_worker_count = 0`), Logger and AccessLogger fall back to their own internal worker threads, MetricsServer runs on its own std::thread, and config reload runs inline in the reactor periodic callback.

### Data-Sharing Rules

The reactor thread is the single owner of all connection maps. Cross-thread communication is minimal:

- **Reactor <-- HealthChecker**: `BackendNode::state_` is an `std::atomic<BackendState>`. The health checker writes it; the reactor reads it for routing decisions.
- **Reactor <-- ThreadPool (config reload)**: The pool worker stores a new config snapshot into `pending_config_` under `pending_config_mutex_`. The reactor consumes it on the next periodic tick and calls `apply_config()`.
- **Reactor --> ThreadPool (log drain)**: The reactor enqueues log entries into `Logger` / `AccessLogger` queues (protected by `std::mutex` + `std::condition_variable`). A pool worker drains the queue to disk.

Full shared-state inventory:

- **`BackendNode::state_`** — `std::atomic<BackendState>`. Written by health checker and reactor (drain/apply). Read by reactor (routing).
- **`BackendNode::active_connections_`** — `std::atomic<uint32_t>`. Written by reactor. Read by reactor and health checker.
- **`Metrics` counters** — `std::atomic<uint64_t>`. Written by reactor. Read by MetricsServer (export).
- **`Metrics` latency buckets** — `std::mutex`. Written by reactor. Read by MetricsServer.
- **Logger / AccessLogger queues** — `std::mutex` + `std::condition_variable`. Written by reactor (enqueue). Read by pool worker or internal thread (drain).
- **`pending_config_`** — `std::mutex`. Written by pool worker. Read by reactor periodic tick.
- **`config_check_in_progress_`** — `std::atomic<bool>`. Written by pool worker and reactor. Read by reactor.
- **`MemoryBudget`** — Lock-free CAS (`std::atomic<uint64_t>`). Written by reactor (reserve/release). Read by reactor and metrics export.
- **`rate_limit_tracker_`** — `std::mutex`. Reactor only.
- **`SessionManager::sessions_`** — `std::mutex`. Reactor only.

### Thread Pool Task Types

**Log drain** (`Logger::drain_queue`)
Posted by `Logger::log()` on every log call, coalesced via the `drain_pending_` atomic flag. Pops all queued log lines and writes to file/stderr. At most one drain task is in-flight at a time.

**Access-log drain** (`AccessLogger::drain_queue`)
Same pattern as Logger, posted by `AccessLogger::log()`.

**Config check + reload**
Posted by the reactor periodic callback every ~1 s (skipped if the previous check is still in-flight). Stats the config file, parses YAML if changed, stores a new snapshot into `pending_config_` for the reactor to apply.

**MetricsServer accept loop**
Posted by `MetricsServer::start()`. Long-running task that blocks on `accept()` with a 100 ms timeout, then posts per-request handler tasks back to the pool.

**MetricsServer request handler**
Posted by the accept loop, one per HTTP request. Reads the request, calls `Metrics::export_prometheus()` or returns health JSON, sends the response, and closes the client fd.


## 3. Connection Lifecycle

### Client Connection

1. **Accept** — `TcpListener::accept()` returns a new client fd.
2. **IP filter** — If the client IP is blacklisted, or a whitelist is active and the IP is not on it, the connection is rejected (HTTP 403 in HTTP mode, or simply closed in TCP mode).
3. **Rate limit** — If the IP exceeds `max_connections` within `window_seconds`, the connection is rejected (HTTP 429 or closed).
4. **Global connection cap** — If the total connection count is at `max_global_connections`:
   - Queue enabled: the fd is placed in a FIFO pending queue (with a `max_wait_ms` timeout).
   - Queue disabled: rejected with HTTP 503 or closed.
5. **TLS handshake** (if `tls_enabled`) — Connection state is set to HANDSHAKE. `SSL_accept()` runs non-blocking, potentially requiring multiple READ/WRITE events. A `tls_handshake_timeout_ms` timer starts. On completion, the fd is removed from `handshake_start_times_`.
6. **Backend connect** — `BackendConnector::connect()` either borrows an idle connection from the pool (validating it first) or opens a new non-blocking socket (state = CONNECTING, then ESTABLISHED on WRITE-ready).
7. **Pairing** — The client and backend `Connection` objects are linked via `set_peer()`. Both fds are registered with the reactor.
8. **Steady-state I/O**:
   - Client READ events forward data to the backend write buffer.
   - Backend READ events forward data to the client write buffer.
   - In HTTP mode, `HttpDataForwarder` parses requests/responses, injects headers (X-Forwarded-For, X-Real-IP, X-Forwarded-Proto), and records `RequestInfo` for access logging.
   - If a write buffer fills, backpressure kicks in: the reactor stops reading from the peer fd (EPOLL mod) and starts a backpressure timeout.
9. **Connection close** — Triggered by any of: client EOF/error, backend EOF/error (triggers retry up to 3x), request timeout, backpressure timeout, or graceful shutdown force-close.
10. **Cleanup** — `ConnectionManager::close_connection()` logs the access entry (HTTP mode), releases the backend connection to the pool if applicable, removes the fd from all tracking maps, decrements `BackendNode::active_connections` and `Metrics::connections_active`, releases the memory budget, and calls `close(fd)`.

### Backend Health

The HealthChecker thread runs in a loop:

1. Sleep for `interval_ms`.
2. For each BackendNode:
   - **TCP mode**: attempt a `connect()` with `timeout_ms`. Success = healthy, failure = unhealthy.
   - **HTTP mode**: connect, send `GET <path> HTTP/1.1`, read response. 2xx = healthy, anything else = unhealthy.
3. Update state:
   - Healthy probe: increment `consecutive_successes`. If it reaches `success_threshold`, mark the backend HEALTHY.
   - Unhealthy probe: increment `consecutive_failures`. If it reaches `failure_threshold`, mark the backend UNHEALTHY.


## 4. Error Model

### Error Categories

**Accept errors** — `accept()` returns -1 (e.g. EMFILE, ENFILE). Logged and silently skipped; the reactor continues.

**Connect errors** — Backend unreachable or connection refused. Up to 3 retries on different backends. If all fail, the client receives HTTP 502 (HTTP mode) or the connection is closed (TCP mode).

**Read/Write errors** — ECONNRESET, EPIPE, TLS SSL_ERROR_SYSCALL, etc. The connection pair is closed immediately.

**TLS handshake errors** — `SSL_accept()` fails or times out. The connection is closed and the error is logged.

**Backend health failures** — A health probe fails `failure_threshold` consecutive times. The backend is marked UNHEALTHY, removed from the routing pool, and re-probed at `interval_ms`.

**Backpressure timeout** — A write buffer stays full past `backpressure_timeout_ms`. Both client and backend connections are force-closed.

**Request timeout** — A connection exceeds `request_timeout_ms`. The connection is closed and `lb_request_timeouts_total` is incremented.

**Memory budget exhaustion** — `MemoryBudget::try_reserve()` returns false. The new connection is rejected with HTTP 503 or closed, and `lb_memory_budget_drops_total` is incremented.

**Rate limit** — An IP exceeds `max_connections` in `window_seconds`. The connection is rejected with HTTP 429 or closed, and `lb_rate_limit_drops_total` is incremented.

**Overload** — The global connection count reaches `max_global_connections` and the queue is full or disabled. The connection is rejected with HTTP 503 or closed, and `lb_overload_drops_total` is incremented.

**Config parse error** — Invalid YAML on reload. The old config remains active; the error is logged; no crash.

**Thread pool task exception** — Any exception thrown inside a `post()` task is caught and swallowed in `ThreadPool::worker_loop()` to keep workers alive.

### Retry Behaviour

When a backend connection fails (connect error or early read error), the `RetryHandler` re-dispatches the client connection to `BackendConnector::connect()` with an incremented retry count. The backend pool selects a different backend on each attempt. After MAX_RETRY_ATTEMPTS (3) failures, the client connection is closed.

### Metrics on Errors

Every error category increments a dedicated Prometheus counter (see `Metrics` class), enabling alerting and dashboarding on:

- `lb_backend_errors_total{backend="..."}`
- `lb_backend_routes_failed_total`
- `lb_request_timeouts_total`
- `lb_rate_limit_drops_total`
- `lb_overload_drops_total`
- `lb_memory_budget_drops_total`
- `lb_backpressure_events_total`
- `lb_backpressure_timeouts_total`
- `lb_bad_requests_total`


## 5. Configuration Lifecycle

### Startup

1. `main()` constructs `ConfigManager` and calls `load_from_file(path)`.
2. `ConfigManager` parses the YAML file into an immutable `std::shared_ptr<const Config>` snapshot.
3. `LoadBalancer::initialize_from_config(config)` reads every field from the snapshot to set up the reactor, listener, backend pool, health checker, TLS context, thread pool, connection pool, memory budget, etc.
4. `set_config_manager()` installs a periodic callback that drives hot-reload.

### Hot Reload

The reactor's periodic callback fires every ~1 second:

1. If a **ThreadPool** is available, it posts a config-check task to the pool. Otherwise, the check runs inline on the reactor thread.
2. The config-check task calls `config_manager_->check_and_reload()`:
   - If the file's mtime is unchanged, it returns immediately.
   - If the mtime changed, it re-parses the YAML and stores the new Config snapshot into `pending_config_` under `pending_config_mutex_`.
3. On the next periodic tick, the reactor checks `pending_config_`. If set, it calls `apply_config()`:
   - Diffs against `last_applied_config_`.
   - Updates routing algorithm, adds/removes/updates backends, reconfigures the health checker, updates rate limits, timeouts, custom headers, and session config.
   - Logs a warning (but skips) for immutable settings that cannot be changed at runtime.

### What Cannot Be Hot-Reloaded

The following settings require a full restart:

- Listener host/port
- Listener mode (tcp / http)
- TLS enable/disable and certificate paths
- Zero-copy splice mode
- Connection pool enable/disable
- Log level / log file paths
- Metrics enable/port
- Thread pool worker count


## 6. Shutdown Sequence

### Signal Handling

`main()` installs `signal_handler` for SIGINT and SIGTERM, which sets the atomic `g_shutdown` flag. A dedicated shutdown-watcher thread polls this flag every 50 ms and calls `LoadBalancer::shutdown_gracefully()`.

### Graceful Shutdown Steps

1. **Close the TcpListener** — `reactor_->del_fd(listener_fd)` followed by `listener_.reset()`. No new connections are accepted from this point.
2. **Mark all backends DRAINING** — No new requests are routed to them.
3. **Set `draining_ = true`** — Set `shutdown_deadline_` to now + `graceful_shutdown_timeout_seconds`.
4. **If 0 active connections** — Shut down immediately.
5. **Otherwise, enter the drain phase** — The reactor periodic callback (every 1 s) calls `check_graceful_shutdown()`:
   - If active connections reach 0: call `force_close_all_connections()`, stop the health checker, stop the reactor.
   - If the deadline is exceeded: log a warning, force-close all remaining connections, stop the health checker, stop the reactor.
   - Otherwise: log progress every 5 seconds ("Draining: N connections remaining, M seconds left").

### Post-Reactor Teardown (in main)

Once `reactor_->run()` returns:

1. **Join the shutdown-watcher thread.**
2. **`metrics_server->stop()`** — Clears the thread pool pointer (so no new tasks are posted), joins the internal thread if any, waits for `pool_task_active_` to go false, and closes the listener socket.
3. **`~LoadBalancer()`** destroys the load balancer:
   - Calls `Logger::set_thread_pool(nullptr)` and `AccessLogger::set_thread_pool(nullptr)` to detach from the pool and re-start their internal worker threads so they can drain remaining entries independently.
   - Stops the health checker.
   - Destroys TLS SSL objects for all connections.
   - Closes all remaining connection fds.
   - `~ThreadPool()` joins all worker threads.
4. **`access_logger.stop()`** — Drains remaining access-log entries.
5. **`logger.stop()`** — Drains remaining log entries.
6. **Process exits.**

### Key Shutdown Invariants

**No dangling pool pointers.** `LoadBalancer::~LoadBalancer()` calls `set_thread_pool(nullptr)` on both loggers before the ThreadPool destructor runs. This detaches the loggers from the pool and re-starts their internal worker threads so they can drain remaining entries independently.

**MetricsServer stops before the pool.** `main()` explicitly stops the metrics server before the LoadBalancer (which owns the pool) is destroyed.

**ThreadPool drains its queue.** `~ThreadPool()` sets `stopping_`, wakes all workers, and joins them. Workers finish their current task before exiting but discard queued tasks.

**Health checker stops cleanly.** `stop()` sets `running_ = false` and joins the health-check thread.

**Reactor stops on drain.** Once all connections are drained (or the deadline passes), `reactor_->stop()` sets `running_ = false`, which causes `epoll_wait` to return and `run()` to exit.

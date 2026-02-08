import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request

import pytest
import yaml
from conftest import wait_for_port


class HoldingBackend:
    """Backend that accepts connections, reads the request, then holds for
    *hold_seconds* before replying.  Keeps the LB→backend connection alive."""

    def __init__(self, port, hold_seconds=30.0):
        self.port = port
        self.hold_seconds = hold_seconds
        self.server_socket = None
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        wait_for_port(self.port, timeout=3.0)

    def stop(self):
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=2.0)

    def _run(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.settimeout(1.0)
        try:
            self.server_socket.bind(("127.0.0.1", self.port))
            self.server_socket.listen(128)
            while self.running:
                try:
                    conn, _ = self.server_socket.accept()
                    threading.Thread(target=self._handle, args=(conn,), daemon=True).start()
                except socket.timeout:
                    continue
                except OSError:
                    break
        finally:
            try:
                self.server_socket.close()
            except OSError:
                pass

    def _handle(self, conn):
        try:
            conn.settimeout(self.hold_seconds + 5.0)
            conn.recv(4096)
            time.sleep(self.hold_seconds)
            body = f"backend-{self.port}"
            resp = (
                f"HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n{body}"
            ).encode()
            conn.sendall(resp)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


class CrashingBackend:
    """Backend that accepts the connection, optionally reads *read_bytes*,
    then abruptly closes without sending a response."""

    def __init__(self, port, read_bytes=0):
        self.port = port
        self.read_bytes = read_bytes
        self.server_socket = None
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        wait_for_port(self.port, timeout=3.0)

    def stop(self):
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=2.0)

    def _run(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.settimeout(1.0)
        try:
            self.server_socket.bind(("127.0.0.1", self.port))
            self.server_socket.listen(5)
            while self.running:
                try:
                    conn, _ = self.server_socket.accept()
                    threading.Thread(target=self._handle, args=(conn,), daemon=True).start()
                except socket.timeout:
                    continue
                except OSError:
                    break
        finally:
            try:
                self.server_socket.close()
            except OSError:
                pass

    def _handle(self, conn):
        try:
            conn.settimeout(2.0)
            if self.read_bytes > 0:
                total = 0
                while total < self.read_bytes:
                    chunk = conn.recv(min(1024, self.read_bytes - total))
                    if not chunk:
                        break
                    total += len(chunk)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def fetch_metrics(port=9090):
    try:
        url = f"http://127.0.0.1:{port}/metrics"
        with urllib.request.urlopen(url, timeout=2.0) as resp:
            return resp.read().decode()
    except (urllib.error.URLError, socket.timeout, ConnectionError, OSError):
        return ""


def get_metric(text, name):
    for line in text.split("\n"):
        line = line.strip()
        if line.startswith(name) and not line.startswith("#"):
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(float(parts[1]))
                except (ValueError, IndexError):
                    pass
    return 0


def write_config(path, config):
    with open(path, "w") as f:
        yaml.dump(config, f)
    return path


def start_lb(lb_binary, config_file, port=None):
    proc = subprocess.Popen(
        [str(lb_binary), str(config_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if port:
        wait_for_port(port, timeout=5.0)
    time.sleep(0.5)
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.fail(f"LB failed to start:\nSTDOUT: {stdout.decode()}\nSTDERR: {stderr.decode()}")
    return proc


def stop_lb(proc):
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def send_http_get(host, port, timeout=5.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect((host, port))
        req = (f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n").encode()
        sock.sendall(req)

        data = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk

        if b"\r\n\r\n" in data:
            _, body = data.split(b"\r\n\r\n", 1)
            return body.decode(errors="ignore").strip()
        return ""
    except (socket.error, Exception):
        return ""
    finally:
        sock.close()


class TestEdgeCases:
    def test_max_connections_rejection(self, lb_binary, tmp_path, backend_ports):
        """Exceeding max_global_connections rejects new clients and
        increments the overload_drops metric."""
        lb_port = 8870
        metrics_port = 9091
        max_conns = 5

        backend = HoldingBackend(backend_ports[0], hold_seconds=30.0)
        backend.start()

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": max_conns,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": True, "port": metrics_port},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        held = []
        try:
            for _ in range(max_conns):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(3.0)
                s.connect(("127.0.0.1", lb_port))
                s.sendall(
                    f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{lb_port}\r\n"
                    f"Connection: keep-alive\r\n\r\n".encode()
                )
                held.append(s)
                time.sleep(0.1)

            time.sleep(0.5)

            rejected = False
            try:
                resp = send_http_get("127.0.0.1", lb_port, timeout=3.0)
                if not resp or not resp.startswith("backend-"):
                    rejected = True
            except (ConnectionRefusedError, ConnectionResetError, socket.timeout, OSError):
                rejected = True

            assert rejected, "Connection beyond max_global_connections should be rejected"

            metrics = fetch_metrics(metrics_port)
            drops = get_metric(metrics, "lb_overload_drops_total")
            assert drops >= 1, f"overload_drops should be ≥1, got {drops}"

        finally:
            for s in held:
                try:
                    s.close()
                except OSError:
                    pass
            stop_lb(proc)
            backend.stop()

    def test_memory_budget_config_applied(
        self, lb_binary, tmp_path, backend_servers, backend_ports
    ):
        """The memory budget limit from the config is applied and
        exposed via the Prometheus gauge metric.  (The actual
        budget enforcement logic is covered by the C++ unit test
        test_memory_budget.cpp.)"""
        lb_port = 8871
        metrics_port = 9092

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": p} for p in backend_ports],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "memory": {"global_buffer_budget_kb": 512},
            "metrics": {"enabled": True, "port": metrics_port},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        try:
            # Wait for at least one periodic callback (1 s)
            time.sleep(1.0)

            metrics = fetch_metrics(metrics_port)
            limit = get_metric(metrics, "lb_memory_budget_limit_bytes")
            assert limit == 512 * 1024, f"budget limit should be 512 KB (524288), got {limit} bytes"

            # A normal request should succeed (budget not exceeded)
            resp = send_http_get("127.0.0.1", lb_port)
            assert resp.startswith("backend-"), f"Normal request should succeed, got: {resp}"

            # Drops counter should still be 0 (normal traffic)
            metrics = fetch_metrics(metrics_port)
            drops = get_metric(metrics, "lb_memory_budget_drops_total")
            assert drops == 0, f"No drops expected for normal traffic, got {drops}"

        finally:
            stop_lb(proc)

    def test_all_backends_unhealthy_rejects(self, lb_binary, tmp_path, backend_ports):
        """When every backend is unhealthy, new requests get no valid
        response."""
        lb_port = 8872

        # Deliberately do NOT start any backends.
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
                {"host": "127.0.0.1", "port": backend_ports[1]},
            ],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        try:
            # Wait for health checks to mark both backends UNHEALTHY
            # (interval=200 ms, threshold=2 → ~0.4 s; add buffer)
            time.sleep(1.5)

            failures = 0
            for _ in range(5):
                resp = send_http_get("127.0.0.1", lb_port, timeout=2.0)
                if not resp or not resp.startswith("backend-"):
                    failures += 1
                time.sleep(0.1)

            assert failures == 5, (
                f"All requests should fail with no healthy backends, got {5 - failures}/5 successes"
            )

        finally:
            stop_lb(proc)

    def test_tls_handshake_timeout(self, lb_binary, tmp_path, backend_ports):
        """A raw TCP connection that never starts the TLS handshake is
        closed after tls_handshake_timeout_ms."""
        lb_port = 8873

        cert = tmp_path / "server.crt"
        key = tmp_path / "server.key"
        subprocess.run(
            ["openssl", "genrsa", "-out", str(key), "2048"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                "openssl",
                "req",
                "-new",
                "-x509",
                "-key",
                str(key),
                "-out",
                str(cert),
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
            ],
            check=True,
            capture_output=True,
        )

        backend = HoldingBackend(backend_ports[0], hold_seconds=30.0)
        backend.start()

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": True,
                "tls_cert": str(cert),
                "tls_key": str(key),
            },
            "tls": {
                "handshake_timeout_ms": 2000,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ],
            "routing": {"algorithm": "round_robin"},
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10.0)
            sock.connect(("127.0.0.1", lb_port))

            try:
                # Hold the socket open without sending any TLS
                # ClientHello.  The periodic callback checks handshake
                # timeouts every ~1 s, so closure happens within
                # timeout_ms + one callback period.
                start_t = time.time()
                data = sock.recv(1)
                elapsed = time.time() - start_t
                # Empty recv means the server closed the connection.
                assert data == b"", "Server should close the stalled TLS handshake"
                assert elapsed < 6.0, f"Timeout should fire within ~3 s, took {elapsed:.1f} s"
            except (ConnectionResetError, BrokenPipeError):
                elapsed = time.time() - start_t
                assert elapsed < 6.0, f"Timeout should fire within ~3 s, took {elapsed:.1f} s"
            except socket.timeout:
                pytest.fail("Stalled TLS handshake was not closed by the server")
            finally:
                try:
                    sock.close()
                except OSError:
                    pass

        finally:
            stop_lb(proc)
            backend.stop()

    def test_backend_crash_mid_request(self, lb_binary, tmp_path, backend_ports):
        """The LB survives when a backend abruptly closes mid-request."""
        lb_port = 8874

        crashing = CrashingBackend(backend_ports[0], read_bytes=100)
        crashing.start()

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 5,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        try:
            for _ in range(5):
                send_http_get("127.0.0.1", lb_port, timeout=3.0)
                time.sleep(0.2)

            assert proc.poll() is None, "LB process should survive backend crashes"

        finally:
            stop_lb(proc)
            crashing.stop()

    def test_malformed_config_reload_keeps_old(
        self, lb_binary, tmp_path, backend_servers, backend_ports
    ):
        """A bad config file is rejected; the old config stays active."""
        lb_port = 8875

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": p} for p in backend_ports],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        write_config(config_file, config)
        proc = start_lb(lb_binary, config_file, port=lb_port)

        try:
            resp = send_http_get("127.0.0.1", lb_port)
            assert resp.startswith("backend-"), f"Should work initially: {resp}"

            # Overwrite with garbage YAML
            time.sleep(1.0)
            with open(config_file, "w") as f:
                f.write("listener:\n  port: [[[INVALID\n  !!garbage!!")

            time.sleep(1.5)

            resp = send_http_get("127.0.0.1", lb_port)
            assert resp.startswith("backend-"), f"Should still work after malformed reload: {resp}"

            assert proc.poll() is None, "LB should survive a malformed config"

        finally:
            stop_lb(proc)

    def test_rapid_connect_disconnect(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """100 rapid open-then-close connections don't crash or leak."""
        lb_port = 8876

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": p} for p in backend_ports],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 1000,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 10000},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        try:
            for _ in range(100):
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.settimeout(0.5)
                    s.connect(("127.0.0.1", lb_port))
                    s.close()
                except (ConnectionRefusedError, socket.timeout, OSError):
                    pass

            time.sleep(1.0)

            assert proc.poll() is None, "LB should survive rapid connect/disconnect"

            successes = 0
            for _ in range(5):
                resp = send_http_get("127.0.0.1", lb_port, timeout=3.0)
                if resp.startswith("backend-"):
                    successes += 1
                time.sleep(0.1)

            assert successes >= 3, (
                f"LB should still handle traffic after churn: {successes}/5 succeeded"
            )

        finally:
            stop_lb(proc)

    def test_drain_shutdown_force_closes(self, lb_binary, tmp_path, backend_ports):
        """In-flight connections are force-closed after the graceful
        shutdown drain timeout expires."""
        lb_port = 8877

        backend = HoldingBackend(backend_ports[0], hold_seconds=120.0)
        backend.start()

        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ],
            "routing": {
                "algorithm": "round_robin",
                "max_connections_per_backend": 100,
                "max_global_connections": 1000,
            },
            "health_check": {
                "interval_ms": 200,
                "timeout_ms": 200,
                "failure_threshold": 2,
                "success_threshold": 1,
                "type": "tcp",
            },
            "backpressure": {"timeout_ms": 30000},
            "graceful_shutdown": {"timeout_seconds": 3},
            "metrics": {"enabled": False},
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        cfg = write_config(tmp_path / "config.yaml", config)
        proc = start_lb(lb_binary, cfg, port=lb_port)

        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect(("127.0.0.1", lb_port))
            sock.sendall(
                f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{lb_port}\r\nConnection: close\r\n\r\n".encode()
            )

            time.sleep(0.5)

            proc.send_signal(signal.SIGTERM)

            # drain timeout = 3 s → process should exit within ~6 s
            try:
                exit_code = proc.wait(timeout=8)
                assert exit_code in [0, 143, -15], f"Unexpected exit code: {exit_code}"
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                pytest.fail("LB did not force-close within drain timeout")

        finally:
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
            try:
                proc.kill()
                proc.wait()
            except OSError:
                pass
            backend.stop()

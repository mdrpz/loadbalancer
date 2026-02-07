"""Tests for backpressure behavior with slow backends."""

import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request

import pytest
import yaml


class SlowBackend:
    """Backend server that reads data slowly to trigger backpressure."""

    def __init__(
        self,
        port,
        read_delay=0.1,
        chunk_size=1024,
        response_delay=0.0,
        rcv_buf_size=None,
    ):
        self.port = port
        self.read_delay = read_delay  # Delay between reads
        self.chunk_size = chunk_size  # Bytes to read per chunk
        self.response_delay = response_delay  # Delay before sending response
        self.rcv_buf_size = rcv_buf_size  # TCP receive buffer size
        self.server_socket = None
        self.running = False
        self.thread = None
        self.connection_count = 0

    def start(self):
        """Start the slow backend server in a separate thread."""
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        time.sleep(0.2)

    def stop(self):
        """Stop the slow backend server."""
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=2.0)

    def _run(self):
        """Run the server loop."""
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if self.rcv_buf_size:
            self.server_socket.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_RCVBUF,
                self.rcv_buf_size,
            )
        self.server_socket.settimeout(1.0)

        try:
            self.server_socket.bind(("127.0.0.1", self.port))
            self.server_socket.listen(5)

            while self.running:
                try:
                    conn, addr = self.server_socket.accept()
                    self.connection_count += 1
                    threading.Thread(
                        target=self._handle_connection, args=(conn,), daemon=True
                    ).start()
                except socket.timeout:
                    continue
                except OSError:
                    break
        except Exception as e:
            print(f"Slow backend server error on port {self.port}: {e}")
        finally:
            if self.server_socket:
                try:
                    self.server_socket.close()
                except OSError:
                    pass

    def _handle_connection(self, conn):
        """Handle a client connection, reading slowly."""
        try:
            if self.rcv_buf_size:
                conn.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_RCVBUF,
                    self.rcv_buf_size,
                )
            conn.settimeout(10.0)
            # Read request slowly in small chunks
            total_read = 0
            while True:
                chunk = conn.recv(self.chunk_size)
                if not chunk:
                    break
                total_read += len(chunk)
                time.sleep(self.read_delay)

            # Wait before responding (if configured)
            if self.response_delay > 0:
                time.sleep(self.response_delay)

            # Send response quickly (to test backpressure on response side)
            body = f"backend-{self.port}"
            response = (
                f"HTTP/1.1 200 OK\r\n"
                f"Content-Type: text/plain\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"Connection: close\r\n"
                f"\r\n"
                f"{body}"
            ).encode()
            conn.sendall(response)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def fetch_metrics(metrics_port=9090):
    """Fetch Prometheus metrics from the load balancer."""
    try:
        url = f"http://127.0.0.1:{metrics_port}/metrics"
        with urllib.request.urlopen(url, timeout=2.0) as response:
            return response.read().decode("utf-8")
    except (urllib.error.URLError, socket.timeout):
        return ""


def get_metric_value(metrics_text, metric_name):
    """Extract a metric value from Prometheus format."""
    for line in metrics_text.split("\n"):
        line = line.strip()
        if line.startswith(metric_name) and not line.startswith("#"):
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(float(parts[1]))
                except (ValueError, IndexError):
                    pass
    return 0


def send_large_request(host, port, size_kb=100, timeout=30.0):
    """Send a large HTTP request to trigger backpressure."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect((host, port))

        # Send a large request body
        body = "x" * (size_kb * 1024)
        request = (
            f"POST /test HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode()
        sock.sendall(request)

        # Send body in chunks to simulate slow client
        chunk_size = 8192
        sent = 0
        while sent < len(body):
            chunk = body[sent : sent + chunk_size].encode()
            sock.sendall(chunk)
            sent += len(chunk)
            time.sleep(0.01)  # Small delay between chunks

        # Read response
        response = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
            if b"\r\n\r\n" in response:
                headers, body_part = response.split(b"\r\n\r\n", 1)
                # Check for Content-Length
                for line in headers.decode().split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        content_length = int(line.split(":", 1)[1].strip())
                        while len(body_part) < content_length:
                            chunk = sock.recv(4096)
                            if not chunk:
                                break
                            body_part += chunk
                        break

        return response.decode("utf-8", errors="ignore")
    except socket.timeout:
        return ""
    except Exception:
        return ""
    finally:
        sock.close()


class TestBackpressure:
    """Tests for backpressure behavior."""

    def test_slow_backend_triggers_backpressure(self, lb_binary, tmp_path, backend_ports):
        """Test that slow backend causes backpressure to activate."""
        # Create config with backpressure timeout and metrics enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": backend_ports[0]}],
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
            "backpressure": {
                "timeout_ms": 5000,  # 5 second timeout
            },
            "metrics": {
                "enabled": True,
                "port": 9090,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start slow backend
        slow_backend = SlowBackend(backend_ports[0], read_delay=0.2, chunk_size=512)
        slow_backend.start()

        # Start load balancer
        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            # Wait for LB to start
            time.sleep(2.0)

            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                pytest.fail(
                    f"Load balancer failed to start:\nSTDOUT: {stdout.decode()}\n"
                    f"STDERR: {stderr.decode()}"
                )

            # Get initial metrics
            initial_metrics = fetch_metrics()
            initial_events = get_metric_value(initial_metrics, "lb_backpressure_events_total")

            # Send a large request that will trigger backpressure
            # Use a separate thread so we can check metrics while request is in progress
            request_complete = threading.Event()
            request_result = [None]

            def make_request():
                try:
                    result = send_large_request("127.0.0.1", 8888, size_kb=50)
                    request_result[0] = result
                except Exception as e:
                    request_result[0] = str(e)
                finally:
                    request_complete.set()

            request_thread = threading.Thread(target=make_request, daemon=True)
            request_thread.start()

            # Wait a bit for backpressure to trigger
            time.sleep(1.0)

            # Check metrics - backpressure should be active
            metrics = fetch_metrics()
            events = get_metric_value(metrics, "lb_backpressure_events_total")

            # Backpressure should have been triggered
            assert events > initial_events, "Backpressure events should have increased"
            # Active backpressure may be 0 if it already cleared, but events should be > 0
            assert events >= 1, "At least one backpressure event should have occurred"

            # Wait for request to complete
            request_complete.wait(timeout=30.0)

            # Check final metrics - backpressure should have cleared
            time.sleep(0.5)
            final_metrics = fetch_metrics()
            final_active = get_metric_value(final_metrics, "lb_backpressure_active")

            # Active backpressure should be 0 after request completes
            assert final_active == 0, "Backpressure should have cleared after request completes"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            slow_backend.stop()

    def test_backpressure_timeout(self, lb_binary, tmp_path, backend_ports):
        """Test that connections timeout if backpressure persists too long."""
        # Create config with short backpressure timeout
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": backend_ports[0]}],
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
            "backpressure": {
                "timeout_ms": 2000,  # 2 second timeout
            },
            "metrics": {
                "enabled": True,
                "port": 9090,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start very slow backend (reads very slowly, doesn't respond)
        very_slow_backend = SlowBackend(
            backend_ports[0], read_delay=1.0, chunk_size=64, response_delay=10.0
        )
        very_slow_backend.start()

        # Start load balancer
        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            # Wait for LB to start
            time.sleep(2.0)

            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                pytest.fail(
                    f"Load balancer failed to start:\nSTDOUT: {stdout.decode()}\n"
                    f"STDERR: {stderr.decode()}"
                )

            # Get initial metrics
            initial_metrics = fetch_metrics()
            initial_timeouts = get_metric_value(initial_metrics, "lb_backpressure_timeouts_total")

            # Send a request that will trigger backpressure timeout
            # The backend is so slow that backpressure will timeout
            send_large_request("127.0.0.1", 8888, size_kb=10, timeout=10.0)

            # Wait for backpressure to be triggered first
            time.sleep(1.0)
            metrics = fetch_metrics()
            events = get_metric_value(metrics, "lb_backpressure_events_total")
            assert events > 0, "Backpressure should have been triggered first"

            # Wait for timeout to occur (2 second timeout + buffer)
            time.sleep(3.0)

            # Check metrics - timeout should have occurred
            metrics = fetch_metrics()
            timeouts = get_metric_value(metrics, "lb_backpressure_timeouts_total")

            # Backpressure timeout should have been triggered
            assert timeouts > initial_timeouts, "Backpressure timeout should have occurred"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            very_slow_backend.stop()

    def test_backpressure_recovery(self, lb_binary, tmp_path, backend_ports):
        """Test that backpressure clears when backend catches up."""
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [{"host": "127.0.0.1", "port": backend_ports[0]}],
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
            "backpressure": {
                "timeout_ms": 30000,
            },
            "metrics": {
                "enabled": True,
                "port": 9090,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Slow backend with tiny TCP receive buffer to guarantee backpressure
        slow_backend = SlowBackend(
            backend_ports[0],
            read_delay=0.05,
            chunk_size=1024,
            rcv_buf_size=4096,
        )
        slow_backend.start()

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            if proc.poll() is not None:
                stdout, stderr = proc.communicate()
                pytest.fail(
                    f"Load balancer failed to start:\n"
                    f"STDOUT: {stdout.decode()}\n"
                    f"STDERR: {stderr.decode()}"
                )

            # Send request asynchronously so we can poll metrics
            request_complete = threading.Event()

            def make_request():
                try:
                    send_large_request(
                        "127.0.0.1",
                        8888,
                        size_kb=50,
                        timeout=60.0,
                    )
                except Exception:
                    pass
                finally:
                    request_complete.set()

            request_thread = threading.Thread(
                target=make_request,
                daemon=True,
            )
            request_thread.start()

            # Poll until backpressure is triggered
            events = 0
            for _ in range(20):
                time.sleep(0.5)
                metrics = fetch_metrics()
                events = get_metric_value(
                    metrics,
                    "lb_backpressure_events_total",
                )
                if events >= 1:
                    break

            assert events >= 1, "Backpressure events should have occurred"

            request_complete.wait(timeout=60.0)

            # Allow connections to fully close
            time.sleep(2.0)

            metrics = fetch_metrics()
            active = get_metric_value(
                metrics,
                "lb_backpressure_active",
            )
            assert active == 0, "Backpressure should have cleared after request completes"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            slow_backend.stop()

"""Tests for zero-copy splice functionality."""

import socket
import subprocess
import sys
import threading
import time

import pytest
import yaml


class EchoBackend:
    """Simple TCP echo server for testing splice."""

    def __init__(self, port):
        self.port = port
        self.server_socket = None
        self.running = False
        self.thread = None
        self.connection_count = 0

    def start(self):
        """Start the echo server in a separate thread."""
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        time.sleep(0.2)

    def stop(self):
        """Stop the echo server."""
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
            print(f"Echo backend server error on port {self.port}: {e}")
        finally:
            if self.server_socket:
                try:
                    self.server_socket.close()
                except OSError:
                    pass

    def _handle_connection(self, conn):
        """Handle a client connection by echoing data."""
        try:
            conn.settimeout(10.0)
            total_received = 0
            while True:
                data = conn.recv(65536)
                if not data:
                    # Client closed connection
                    break
                total_received += len(data)
                # Echo the data back immediately
                sent = 0
                while sent < len(data):
                    try:
                        bytes_sent = conn.send(data[sent:])
                        if bytes_sent == 0:
                            break
                        sent += bytes_sent
                    except (OSError, socket.error):
                        break
                # Small delay to ensure data is processed
                time.sleep(0.01)
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def send_tcp_data(host, port, data, timeout=10.0):
    """Send TCP data and receive echo."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect((host, port))
        sock.sendall(data)
        # Don't shutdown write side immediately - keep connection open for echo
        # The backend will echo the data back, then we can close

        received = b""
        # Set a shorter timeout for reading to avoid hanging
        sock.settimeout(5.0)
        start_time = time.time()
        while True:
            try:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                received += chunk
                # If we've received as much as we sent, we're likely done
                if len(received) >= len(data):
                    # Give a small window for any trailing data
                    time.sleep(0.1)
                    try:
                        sock.settimeout(0.1)
                        extra = sock.recv(65536)
                        if extra:
                            received += extra
                    except socket.timeout:
                        pass
                    break
                # Safety timeout
                if time.time() - start_time > 4.0:
                    break
            except socket.timeout:
                # No more data available
                break

        return received
    except Exception:
        return b""
    finally:
        try:
            sock.close()
        except Exception:
            pass


@pytest.mark.skipif(sys.platform != "linux", reason="Splice only works on Linux")
class TestSplice:
    """Tests for zero-copy splice functionality."""

    def test_splice_enabled_tcp_mode(self, lb_binary, tmp_path, backend_ports):
        """Test splice is used in TCP mode when enabled."""
        # Create config with TCP mode and splice enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "tcp",
                "use_splice": True,  # Enable splice
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
            "metrics": {
                "enabled": False,  # Disable metrics for this test
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start echo backend
        echo_backend = EchoBackend(backend_ports[0])
        echo_backend.start()

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

            # Send data and verify echo
            test_data = b"Hello, World! This is a test message for splice."
            received = send_tcp_data("127.0.0.1", 8888, test_data)

            # Verify data integrity
            assert received == test_data, (
                f"Data mismatch: sent {len(test_data)} bytes, received {len(received)} bytes"
            )

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            echo_backend.stop()

    def test_splice_disabled_fallback(self, lb_binary, tmp_path, backend_ports):
        """Test fallback to buffer forwarding when splice disabled."""
        # Create config with TCP mode but splice disabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "tcp",
                "use_splice": False,  # Disable splice
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
            "metrics": {
                "enabled": False,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start echo backend
        echo_backend = EchoBackend(backend_ports[0])
        echo_backend.start()

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

            # Send data and verify echo (should work with buffer forwarding too)
            test_data = b"Hello, World! This is a test message for buffer forwarding."
            received = send_tcp_data("127.0.0.1", 8888, test_data)

            # Verify data integrity
            assert received == test_data, (
                f"Data mismatch: sent {len(test_data)} bytes, received {len(received)} bytes"
            )

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            echo_backend.stop()

    def test_splice_large_transfer(self, lb_binary, tmp_path, backend_ports):
        """Test splice handles large data transfers correctly."""
        # Create config with TCP mode and splice enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "tcp",
                "use_splice": True,
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
            "metrics": {
                "enabled": False,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start echo backend
        echo_backend = EchoBackend(backend_ports[0])
        echo_backend.start()

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

            # Send large data (1MB)
            large_data = b"x" * (1024 * 1024)
            received = send_tcp_data("127.0.0.1", 8888, large_data, timeout=30.0)

            # Verify data integrity
            assert len(received) == len(large_data), (
                f"Size mismatch: sent {len(large_data)} bytes, received {len(received)} bytes"
            )
            assert received == large_data, "Data content mismatch"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            echo_backend.stop()

    def test_splice_multiple_connections(self, lb_binary, tmp_path, backend_ports):
        """Test splice works correctly with multiple concurrent connections."""
        # Create config with TCP mode and splice enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": 8888,
                "mode": "tcp",
                "use_splice": True,
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
            "metrics": {
                "enabled": False,
            },
            "logging": {
                "level": "error",
                "file": str(tmp_path / "lb.log"),
            },
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        # Start echo backend
        echo_backend = EchoBackend(backend_ports[0])
        echo_backend.start()

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

            # Send multiple concurrent requests
            results = []
            errors = []
            lock = threading.Lock()

            def send_request(i):
                try:
                    test_data = f"Request {i}: " + "x" * 1000
                    received = send_tcp_data("127.0.0.1", 8888, test_data.encode())
                    with lock:
                        if received == test_data.encode():
                            results.append(True)
                        else:
                            errors.append(f"Request {i}: data mismatch")
                except Exception as e:
                    with lock:
                        errors.append(f"Request {i}: {str(e)}")

            threads = [threading.Thread(target=send_request, args=(i,)) for i in range(10)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=10.0)

            # Verify all requests succeeded
            assert len(errors) == 0, f"Errors during concurrent requests: {errors}"
            assert len(results) == 10, f"Expected 10 successful requests, got {len(results)}"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            echo_backend.stop()

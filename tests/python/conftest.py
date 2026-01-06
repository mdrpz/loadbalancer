"""
Pytest configuration and fixtures for load balancer integration tests.
"""

import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest
import yaml


@pytest.fixture(scope="session")
def lb_binary():
    """Path to the load balancer binary."""
    binary = Path(__file__).parent.parent.parent / "build" / "lb"
    if not binary.exists():
        pytest.skip("Load balancer binary not found. Run 'cmake --build build' first.")
    return binary


def create_test_config(
    tmp_path, lb_port, backend_ports, routing_algorithm="round_robin", tls_enabled=False
):
    """Create a minimal test config file."""
    config = {
        "listener": {
            "host": "127.0.0.1",
            "port": lb_port,
            "mode": "http",
            "tls_enabled": tls_enabled,
        },
        "backends": [{"host": "127.0.0.1", "port": port} for port in backend_ports],
        "routing": {
            "algorithm": routing_algorithm,
            "max_connections_per_backend": 100,
            "max_global_connections": 1000,
        },
        "health_check": {
            "interval_ms": 200,  # Very fast for tests
            "timeout_ms": 200,
            "failure_threshold": 2,
            "success_threshold": 1,  # Mark healthy after 1 successful check
            "type": "tcp",
        },
        "backpressure": {
            "timeout_ms": 10000,
        },
        "metrics": {
            "enabled": False,  # Disable metrics for tests to avoid port conflicts
        },
        "logging": {
            "level": "error",  # Reduce noise in tests
            "file": str(tmp_path / "lb.log"),
        },
    }

    config_file = tmp_path / "config.yaml"
    with open(config_file, "w") as f:
        yaml.dump(config, f)
    return config_file


class BackendServer:
    """Simple HTTP backend server that identifies itself by port."""

    def __init__(self, port):
        self.port = port
        self.server_socket = None
        self.running = False
        self.thread = None
        self.connection_count = 0

    def start(self):
        """Start the backend server in a separate thread."""
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        time.sleep(0.2)

    def stop(self):
        """Stop the backend server."""
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass
        if self.thread:
            self.thread.join(timeout=1.0)

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
            print(f"Backend server error on port {self.port}: {e}")
        finally:
            if self.server_socket:
                try:
                    self.server_socket.close()
                except OSError:
                    pass

    def _handle_connection(self, conn):
        """Handle a client connection."""
        try:
            conn.settimeout(5.0)
            data = conn.recv(4096)
            if data:
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


@pytest.fixture
def backend_ports():
    """Default backend ports for testing."""
    return [9000, 9001]


@pytest.fixture
def backend_servers(backend_ports):
    """Create and start backend servers."""
    servers = [BackendServer(port) for port in backend_ports]
    for server in servers:
        server.start()

    yield servers

    for server in servers:
        server.stop()


@pytest.fixture
def lb_port():
    """Default load balancer port for testing."""
    return 8888


@pytest.fixture
def lb_process(lb_binary, tmp_path, lb_port, backend_ports, backend_servers):
    """Start load balancer process and yield, then cleanup.

    Depends on backend_servers to ensure backends are running before LB starts.
    """
    config_file = create_test_config(tmp_path, lb_port, backend_ports)

    proc = subprocess.Popen(
        [str(lb_binary), str(config_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    # Wait for LB to start and health checks to mark backends healthy
    # With 200ms interval and success_threshold=1, this should be enough
    time.sleep(2.0)

    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.fail(
            f"Load balancer failed to start:\nSTDOUT: {stdout.decode()}\nSTDERR: {stderr.decode()}"
        )

    yield proc

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

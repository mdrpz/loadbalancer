"""Tests for TLS termination."""

import socket
import ssl
import subprocess
import time

import yaml
from conftest import send_http_request


class TestTLS:
    """Tests for TLS termination."""

    def _generate_test_certs(self, tmp_path):
        """Generate self-signed test certificates."""
        cert_file = tmp_path / "server.crt"
        key_file = tmp_path / "server.key"

        # Generate private key
        subprocess.run(
            ["openssl", "genrsa", "-out", str(key_file), "2048"],
            check=True,
            capture_output=True,
        )

        # Generate self-signed certificate
        subprocess.run(
            [
                "openssl",
                "req",
                "-new",
                "-x509",
                "-key",
                str(key_file),
                "-out",
                str(cert_file),
                "-days",
                "1",
                "-subj",
                "/CN=localhost",
            ],
            check=True,
            capture_output=True,
        )

        return cert_file, key_file

    def _send_https_request(self, host, port, path="/"):
        """Send HTTPS request with SSL, ignoring certificate verification."""
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.settimeout(5.0)
            ssl_sock = context.wrap_socket(sock, server_hostname=host)
            ssl_sock.connect((host, port))

            request = f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n"
            ssl_sock.sendall(request.encode())

            response = b""
            while True:
                chunk = ssl_sock.recv(4096)
                if not chunk:
                    break
                response += chunk

            if b"\r\n\r\n" in response:
                _, body = response.split(b"\r\n\r\n", 1)
                return body.decode().strip()
            return ""
        except Exception:
            return ""
        finally:
            sock.close()

    def test_tls_termination(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that TLS connections are terminated and forwarded to backends."""
        lb_port = 8895

        # Generate test certificates
        cert_file, key_file = self._generate_test_certs(tmp_path)

        # Create config with TLS enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": True,
                "tls_cert": str(cert_file),
                "tls_key": str(key_file),
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
            "logging": {"level": "error", "file": str(tmp_path / "lb.log")},
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            # Send HTTPS request
            response = self._send_https_request("127.0.0.1", lb_port)
            assert response.startswith("backend-"), f"TLS request failed: {response}"

            # Send multiple requests to verify routing works
            responses = []
            for _ in range(10):
                resp = self._send_https_request("127.0.0.1", lb_port)
                if resp.startswith("backend-"):
                    responses.append(resp)
                time.sleep(0.05)

            assert len(responses) >= 8, f"Expected most TLS requests to succeed: {responses}"

            # Verify round-robin works over TLS
            backends = set(responses)
            assert len(backends) >= 2, f"Should route to multiple backends: {backends}"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def test_plain_http_rejected_when_tls_enabled(
        self, lb_binary, tmp_path, backend_servers, backend_ports
    ):
        """Test that plain HTTP is rejected when TLS is enabled."""
        lb_port = 8896

        # Generate test certificates
        cert_file, key_file = self._generate_test_certs(tmp_path)

        # Create config with TLS enabled
        config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": True,
                "tls_cert": str(cert_file),
                "tls_key": str(key_file),
            },
            "backends": [{"host": "127.0.0.1", "port": backend_ports[0]}],
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
            "logging": {"level": "error", "file": str(tmp_path / "lb.log")},
        }

        config_file = tmp_path / "config.yaml"
        with open(config_file, "w") as f:
            yaml.dump(config, f)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            # Plain HTTP should fail (LB expects TLS handshake)
            response = send_http_request("127.0.0.1", lb_port)
            assert not response.startswith("backend-"), (
                f"Plain HTTP should fail when TLS enabled: {response}"
            )

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

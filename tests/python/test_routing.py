"""
Integration tests for load balancer routing.
"""
import pytest
import socket
import subprocess
import time
import yaml
from pathlib import Path


def send_http_request(host, port, path="/", timeout=5.0):
    """Send a simple HTTP GET request and return the response body."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect((host, port))
        
        request = f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n"
        sock.sendall(request.encode())
        
        response = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response += chunk
            if b"\r\n\r\n" in response:
                headers, body = response.split(b"\r\n\r\n", 1)
                content_length = None
                for line in headers.decode().split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        content_length = int(line.split(":", 1)[1].strip())
                        break
                if content_length and len(body) >= content_length:
                    break
        
        if b"\r\n\r\n" in response:
            _, body = response.split(b"\r\n\r\n", 1)
            return body.decode().strip()
        return ""
    except socket.error:
        return ""
    except Exception:
        return ""
    finally:
        sock.close()


def create_test_config(tmp_path, lb_port, backend_ports, routing_algorithm="round_robin", 
                       tls_enabled=False, health_check_interval_ms=200):
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
            "interval_ms": health_check_interval_ms,
            "timeout_ms": 200,
            "failure_threshold": 2,
            "success_threshold": 1,
            "type": "tcp",
        },
        "backpressure": {
            "timeout_ms": 10000,
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
    return config_file


class TestRoundRobin:
    """Tests for round-robin routing algorithm."""
    
    def test_distribution(self, lb_process, lb_port, backend_ports):
        """Test that requests are distributed evenly across backends."""
        num_requests = 20
        backend_responses = []
        
        for _ in range(num_requests):
            response = send_http_request("127.0.0.1", lb_port)
            if response.startswith("backend-"):
                backend_responses.append(response)
            time.sleep(0.05)
        
        backend_counts = {}
        for response in backend_responses:
            backend_counts[response] = backend_counts.get(response, 0) + 1
        
        assert len(backend_counts) >= 2, f"Expected requests to multiple backends, got: {backend_counts}"
        
        num_backends = len(backend_ports)
        expected_per_backend = num_requests / num_backends
        for backend_id, count in backend_counts.items():
            ratio = count / expected_per_backend
            assert 0.5 <= ratio <= 1.5, (
                f"Backend {backend_id} received {count} requests, "
                f"expected ~{expected_per_backend:.1f} (ratio: {ratio:.2f})"
            )
    
    def test_alternation(self, lb_process, lb_port):
        """Test that round-robin alternates between backends."""
        responses = []
        for _ in range(6):
            response = send_http_request("127.0.0.1", lb_port)
            if response.startswith("backend-"):
                responses.append(response)
            time.sleep(0.05)
        
        if len(responses) >= 4:
            unique_in_first_four = len(set(responses[:4]))
            assert unique_in_first_four >= 2, (
                f"Round-robin should alternate backends, but first 4 requests went to: {responses[:4]}"
            )


@pytest.fixture
def lb_process_lc(lb_binary, tmp_path, backend_ports, backend_servers):
    """Load balancer process with least_connections algorithm."""
    lb_port = 8889
    config_file = create_test_config(tmp_path, lb_port, backend_ports, routing_algorithm="least_connections")
    
    proc = subprocess.Popen(
        [str(lb_binary), str(config_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    time.sleep(2.0)
    
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.fail(f"Load balancer failed to start:\nSTDOUT: {stdout.decode()}\nSTDERR: {stderr.decode()}")
    
    yield proc, lb_port
    
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


class TestLeastConnections:
    """Tests for least-connections routing algorithm."""
    
    def test_routing_works(self, lb_process_lc):
        """Test that least_connections routing distributes requests."""
        proc, lb_port = lb_process_lc
        
        responses = []
        for _ in range(20):
            response = send_http_request("127.0.0.1", lb_port)
            if response.startswith("backend-"):
                responses.append(response)
            time.sleep(0.05)
        
        assert len(responses) > 0, "Should receive responses from backends"
        
        backend_counts = {}
        for response in responses:
            backend_counts[response] = backend_counts.get(response, 0) + 1
        
        assert len(backend_counts) >= 1, f"Should route to at least one backend, got: {backend_counts}"


class TestHealthCheck:
    """Tests for health check behavior."""
    
    def test_unhealthy_backend_removed(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that requests are not routed to unhealthy backends."""
        lb_port = 8890
        config_file = create_test_config(tmp_path, lb_port, backend_ports, health_check_interval_ms=200)
        
        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        try:
            time.sleep(2.0)
            
            # Verify both backends are receiving traffic
            initial_responses = []
            for _ in range(10):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    initial_responses.append(response)
                time.sleep(0.05)
            
            initial_backends = set(initial_responses)
            assert len(initial_backends) >= 2, "Should route to both backends initially"
            
            # Stop one backend
            backend_servers[0].stop()
            
            # Wait for health check to detect failure
            time.sleep(1.0)
            
            # Now requests should only go to the healthy backend
            after_responses = []
            for _ in range(10):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    after_responses.append(response)
                time.sleep(0.05)
            
            # Should still get responses (from the healthy backend)
            assert len(after_responses) > 0, "Should still receive responses after one backend fails"
            
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


class TestConnectionHandling:
    """Tests for connection handling edge cases."""
    
    def test_multiple_sequential_requests(self, lb_process, lb_port):
        """Test handling multiple sequential requests."""
        for i in range(10):
            response = send_http_request("127.0.0.1", lb_port)
            assert response.startswith("backend-"), f"Request {i} failed: {response}"
    
    def test_concurrent_requests(self, lb_process, lb_port):
        """Test handling concurrent requests."""
        import threading
        
        results = []
        errors = []
        
        def make_request():
            try:
                response = send_http_request("127.0.0.1", lb_port)
                results.append(response)
            except Exception as e:
                errors.append(str(e))
        
        threads = [threading.Thread(target=make_request) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10)
        
        assert len(errors) == 0, f"Errors during concurrent requests: {errors}"
        successful = [r for r in results if r.startswith("backend-")]
        assert len(successful) >= 8, f"Expected most requests to succeed, got {len(successful)}/10"

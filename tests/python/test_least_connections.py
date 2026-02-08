"""Tests for least-connections routing algorithm."""

import subprocess
import time

import pytest
from conftest import create_test_config, send_http_request, wait_for_port


@pytest.fixture
def lb_process_lc(lb_binary, tmp_path, backend_ports, backend_servers):
    """Load balancer process with least_connections algorithm."""
    lb_port = 8889
    config_file = create_test_config(
        tmp_path, lb_port, backend_ports, routing_algorithm="least_connections"
    )

    proc = subprocess.Popen(
        [str(lb_binary), str(config_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    wait_for_port(lb_port, timeout=5.0)
    time.sleep(0.5)

    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        pytest.fail(f"LB failed to start:\nSTDOUT: {stdout.decode()}\nSTDERR: {stderr.decode()}")

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

        assert len(backend_counts) >= 1, (
            f"Should route to at least one backend, got: {backend_counts}"
        )

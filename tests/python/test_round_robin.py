"""Tests for round-robin routing algorithm."""

import time

from conftest import send_http_request


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

        assert len(backend_counts) >= 2, (
            f"Expected requests to multiple backends, got: {backend_counts}"
        )

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
            assert unique_in_first_four >= 2, f"Round-robin should alternate, got: {responses[:4]}"

"""Tests for connection handling edge cases."""

import threading

from conftest import send_http_request


class TestConnectionHandling:
    """Tests for connection handling edge cases."""

    def test_multiple_sequential_requests(self, lb_process, lb_port):
        """Test handling multiple sequential requests."""
        for i in range(10):
            response = send_http_request("127.0.0.1", lb_port)
            assert response.startswith("backend-"), f"Request {i} failed: {response}"

    def test_concurrent_requests(self, lb_process, lb_port):
        """Test handling concurrent requests."""
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

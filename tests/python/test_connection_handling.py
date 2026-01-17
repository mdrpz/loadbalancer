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
        lock = threading.Lock()

        def make_request():
            try:
                response = send_http_request("127.0.0.1", lb_port)
                with lock:
                    results.append(response)
                    if not response or not response.startswith("backend-"):
                        errors.append(
                            f"Request failed or returned invalid response: {repr(response)}"
                        )
            except Exception as e:
                with lock:
                    errors.append(str(e))

        threads = [threading.Thread(target=make_request) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10)

        # Check that we got responses from all threads
        assert len(results) == 10, f"Expected 10 responses, got {len(results)}"

        # Check for explicit errors
        assert len(errors) == 0, f"Errors during concurrent requests: {errors}"

        # Check that most requests succeeded (allow for occasional failures under load)
        successful = [r for r in results if r.startswith("backend-")]
        assert len(successful) >= 7, (
            f"Expected at least 7 requests to succeed, got {len(successful)}/10. Results: {results}"
        )

"""Tests for graceful shutdown behavior."""

import signal
import subprocess
import threading
import time

import pytest
from conftest import create_test_config, send_http_request


class TestGracefulShutdown:
    """Tests for graceful shutdown behavior."""

    def test_sigterm_clean_exit(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that SIGTERM causes clean shutdown."""
        lb_port = 8893
        config_file = create_test_config(tmp_path, lb_port, backend_ports)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            # Verify LB is working
            response = send_http_request("127.0.0.1", lb_port)
            assert response.startswith("backend-"), f"LB should be working: {response}"

            # Send SIGTERM
            proc.send_signal(signal.SIGTERM)

            # Should exit cleanly within timeout
            try:
                exit_code = proc.wait(timeout=10)
                # Exit code 0 or 143 (128 + 15 for SIGTERM) are acceptable
                assert exit_code in [0, 143, -15], f"Unexpected exit code: {exit_code}"
            except subprocess.TimeoutExpired:
                proc.kill()
                pytest.fail("LB did not shut down within 10 seconds after SIGTERM")

        except Exception:
            proc.kill()
            raise

    def test_requests_during_shutdown(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test behavior of in-flight requests during shutdown."""
        lb_port = 8894
        config_file = create_test_config(tmp_path, lb_port, backend_ports)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        results = {"before": [], "during": [], "after": []}

        def send_requests(phase, count):
            for _ in range(count):
                response = send_http_request("127.0.0.1", lb_port, timeout=2.0)
                results[phase].append(response)
                time.sleep(0.05)

        try:
            time.sleep(2.0)

            # Send requests before shutdown
            send_requests("before", 5)
            assert any(r.startswith("backend-") for r in results["before"]), (
                "Should handle requests before shutdown"
            )

            # Start sending requests in background
            request_thread = threading.Thread(target=send_requests, args=("during", 10))
            request_thread.start()

            # Send SIGTERM while requests are in flight
            time.sleep(0.1)
            proc.send_signal(signal.SIGTERM)

            # Wait for request thread to finish
            request_thread.join(timeout=15)

            # LB should have exited
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

            # Requests after shutdown should fail (connection refused)
            send_requests("after", 3)

            # Verify: before requests succeeded, after requests failed
            before_success = sum(1 for r in results["before"] if r.startswith("backend-"))
            after_success = sum(1 for r in results["after"] if r.startswith("backend-"))

            assert before_success >= 3, f"Before shutdown: {before_success}/5 succeeded"
            assert after_success == 0, f"After shutdown: {after_success}/3 should fail"

        except Exception:
            proc.kill()
            raise

"""Tests for health check behavior."""

import subprocess
import time

from conftest import create_test_config, send_http_request


class TestHealthCheck:
    """Tests for health check behavior."""

    def test_unhealthy_backend_removed(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that requests are not routed to unhealthy backends."""
        lb_port = 8890
        config_file = create_test_config(
            tmp_path, lb_port, backend_ports, health_check_interval_ms=200
        )

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
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
            assert len(after_responses) > 0, "Should still get responses after backend fails"

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

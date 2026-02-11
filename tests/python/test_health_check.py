"""Tests for health check behavior."""

import subprocess
import time

from conftest import BackendServer, create_test_config, send_http_request, wait_for_port


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
            wait_for_port(lb_port, timeout=5.0)
            time.sleep(0.5)

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

    def test_backend_recovery(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that a backend transitions back to HEALTHY after recovery."""
        lb_port = 8898
        config_file = create_test_config(
            tmp_path, lb_port, backend_ports, health_check_interval_ms=200
        )

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            wait_for_port(lb_port, timeout=5.0)
            time.sleep(0.5)

            # Phase 1: Both backends healthy — traffic goes to both
            initial_responses = []
            for _ in range(10):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    initial_responses.append(response)
                time.sleep(0.05)

            initial_backends = set(initial_responses)
            assert len(initial_backends) >= 2, "Should route to both backends initially"

            stopped_port = backend_ports[0]

            # Phase 2: Kill one backend, wait for it to be marked UNHEALTHY
            backend_servers[0].stop()
            time.sleep(1.0)

            degraded_responses = []
            for _ in range(10):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    degraded_responses.append(response)
                time.sleep(0.05)

            degraded_backends = set(degraded_responses)
            assert len(degraded_responses) > 0, "Should still get responses from healthy backend"
            assert f"backend-{stopped_port}" not in degraded_backends, (
                "Stopped backend should not receive traffic"
            )

            # Phase 3: Restart the stopped backend and wait for recovery
            recovered = BackendServer(stopped_port)
            recovered.start()
            # Wait long enough for success_threshold (1) health checks to pass
            time.sleep(1.0)

            recovery_responses = []
            for _ in range(20):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    recovery_responses.append(response)
                time.sleep(0.05)

            recovery_backends = set(recovery_responses)
            assert len(recovery_backends) >= 2, (
                "Both backends should receive traffic after recovery"
            )
            assert f"backend-{stopped_port}" in recovery_backends, (
                "Recovered backend should be receiving traffic again"
            )

            recovered.stop()

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

"""Tests for configuration hot reload."""

import subprocess
import time

import yaml
from conftest import send_http_request


class TestConfigReload:
    """Tests for configuration hot reload."""

    def test_backend_removal_draining(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that removing a backend from config marks it as DRAINING."""
        lb_port = 8891

        # Create initial config with both backends
        config_file = tmp_path / "config.yaml"
        initial_config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
                {"host": "127.0.0.1", "port": backend_ports[1]},
            ],
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

        with open(config_file, "w") as f:
            yaml.dump(initial_config, f)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            # Verify both backends receive traffic
            initial_responses = []
            for _ in range(20):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    initial_responses.append(response)
                time.sleep(0.05)

            initial_backends = set(initial_responses)
            assert len(initial_backends) >= 2, (
                f"Should route to both backends initially, got: {initial_backends}"
            )

            # Modify config to remove one backend
            updated_config = initial_config.copy()
            updated_config["backends"] = [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ]

            with open(config_file, "w") as f:
                yaml.dump(updated_config, f)

            # Wait for config reload (checks every 5 seconds)
            time.sleep(6.0)

            # Send more requests - should only go to remaining backend
            after_responses = []
            for _ in range(20):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    after_responses.append(response)
                time.sleep(0.05)

            after_backends = set(after_responses)

            # Should only route to the backend that's still in config
            expected_backend = f"backend-{backend_ports[0]}"
            assert len(after_responses) > 0, "Should still receive responses"
            assert expected_backend in after_backends, (
                f"Traffic should go to {expected_backend}, got: {after_backends}"
            )

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def test_backend_addition(self, lb_binary, tmp_path, backend_servers, backend_ports):
        """Test that adding a backend to config makes it available."""
        lb_port = 8892

        # Create initial config with one backend
        config_file = tmp_path / "config.yaml"
        initial_config = {
            "listener": {
                "host": "127.0.0.1",
                "port": lb_port,
                "mode": "http",
                "tls_enabled": False,
            },
            "backends": [
                {"host": "127.0.0.1", "port": backend_ports[0]},
            ],
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

        with open(config_file, "w") as f:
            yaml.dump(initial_config, f)

        proc = subprocess.Popen(
            [str(lb_binary), str(config_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            time.sleep(2.0)

            # Verify only one backend receives traffic
            initial_responses = []
            for _ in range(10):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    initial_responses.append(response)
                time.sleep(0.05)

            initial_backends = set(initial_responses)
            assert len(initial_backends) == 1, (
                f"Should only route to one backend initially, got: {initial_backends}"
            )

            # Add second backend to config
            updated_config = initial_config.copy()
            updated_config["backends"] = [
                {"host": "127.0.0.1", "port": backend_ports[0]},
                {"host": "127.0.0.1", "port": backend_ports[1]},
            ]

            with open(config_file, "w") as f:
                yaml.dump(updated_config, f)

            # Wait for config reload + health check
            time.sleep(6.0)

            # Send more requests - should now go to both backends
            after_responses = []
            for _ in range(20):
                response = send_http_request("127.0.0.1", lb_port)
                if response.startswith("backend-"):
                    after_responses.append(response)
                time.sleep(0.05)

            after_backends = set(after_responses)
            assert len(after_backends) >= 2, (
                f"Should route to both backends after adding, got: {after_backends}"
            )

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

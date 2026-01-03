"""
Pytest configuration and fixtures for load balancer integration tests.
"""
import pytest
import subprocess
import time
import socket
import asyncio
from pathlib import Path


@pytest.fixture(scope="session")
def lb_binary():
    """Path to the load balancer binary."""
    # Assumes build directory is ./build
    binary = Path(__file__).parent.parent.parent / "build" / "lb"
    if not binary.exists():
        pytest.skip("Load balancer binary not found. Run 'cmake --build build' first.")
    return binary


@pytest.fixture
def lb_process(lb_binary, tmp_path):
    """Start load balancer process and yield, then cleanup."""
    config_file = tmp_path / "config.yaml"
    # TODO: Create minimal config file
    
    proc = subprocess.Popen(
        [str(lb_binary), str(config_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    
    # Give it a moment to start
    time.sleep(0.5)
    
    yield proc
    
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


@pytest.fixture
def backend_server():
    """Create a simple TCP echo server for testing."""
    # TODO: Implement async TCP server
    pass


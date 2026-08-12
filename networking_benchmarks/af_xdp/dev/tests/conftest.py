"""
Fixtures shared across test modules.

Ports are deliberately off the production defaults (control 12345, ucast data
5000, rtt local 19020) so the suite can run on a host with a live
replicator.service without colliding. The control port is exported via
AFXDP_CONTROL_PORT so the kernel-mode replicator, rtt and replicator_ctl
all agree (see src/ControlPort.hpp).
"""

import os
from pathlib import Path

import pytest

# ── Non-production ports ──────────────────────────────────────────────────────
CONTROL_PORT = 23456   # prod: 12345
DATA_PORT = 29000      # prod ucast data: 5000

# Export BEFORE any binary is spawned so all inherit the same control port.
os.environ["AFXDP_CONTROL_PORT"] = str(CONTROL_PORT)

# dev/tests/ → af_xdp root is three levels up (binaries land at the root after `make`).
AF_XDP_DIR = Path(__file__).parent.parent.parent
REPLICATOR = AF_XDP_DIR / "replicator"

LISTEN_IP = "127.0.0.1"


@pytest.fixture(scope="session", autouse=True)
def check_binaries():
    """Fail fast if binaries aren't built."""
    if not REPLICATOR.exists():
        pytest.skip(
            f"Binaries not found at {AF_XDP_DIR}. "
            f"Run 'make all' (or 'make kernel-mode') in {AF_XDP_DIR} first.",
            allow_module_level=True,
        )

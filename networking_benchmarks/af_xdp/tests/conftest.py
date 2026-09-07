"""
Fixtures shared across test modules.

Ports are deliberately off the production defaults (control 12345, ucast data
5000, rtt local 19020) so the suite can run on a host with a live
replicator.service without colliding. The control port is exported via
AFXDP_CONTROL_PORT so the echo-mode replicator, rtt and replicator_ctl
all agree (see src/common/ControlPort.hpp).
"""

import os
from pathlib import Path

import pytest

# ── Non-production ports ──────────────────────────────────────────────────────
CONTROL_PORT = 23456   # prod: 12345
DATA_PORT = 29000      # prod ucast data: 5000

# Export BEFORE any binary is spawned so all inherit the same control port.
os.environ["AFXDP_CONTROL_PORT"] = str(CONTROL_PORT)

# tests/ → af_xdp root is two levels up (binaries land at the root after `make`).
AF_XDP_DIR = Path(__file__).parent.parent
REPLICATOR = AF_XDP_DIR / "replicator"

LISTEN_IP = "127.0.0.1"


@pytest.fixture(autouse=True)
def check_binaries(request):
    """Skip suites that need the C++ binaries when they are not built.

    Suites marked no_cpp_binaries exercise the Go/SQLite surfaces and run
    anywhere, so a host that cannot build the datapath (macOS arm64 rejects
    -mfma) still gets useful coverage instead of an all-skipped run.
    """
    if REPLICATOR.exists():
        return
    if request.node.get_closest_marker("no_cpp_binaries"):
        return
    pytest.skip(
        f"Binaries not found at {AF_XDP_DIR}. "
        f"Run 'make all' (or 'make echo-mode') in {AF_XDP_DIR} first."
    )

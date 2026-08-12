# tests/

Integration tests for the AF_XDP benchmark suite (pytest).

They exercise the **real** compiled binaries — `rtt`, `replicator_ctl`, `udp_send`,
`mcast_send`, `mcast_receive` — and the control protocol against an **echo-mode
replicator** (`replicator --echo-mode`). Echo mode implements the same ADD / REMOVE /
LIST control-port contract and UDP echo as the production AF_XDP replicator, but over
standard kernel sockets: no root, no XDP/BPF, no NIC. That makes the suite runnable in
containers, CI, and macOS.

The production AF_XDP zero-copy datapath itself is validated separately on EC2 via
`deploy/ansible/run_ucast.yaml` (unicast) and `deploy/ansible/run_mcast.yaml`
(multicast-over-unicast).

![Test execution overview](assets/test-execution-overview.svg)

---

## Port Isolation

Non-production ports are used so the suite never collides with a live
`replicator.service`:

| Port | Test value | Production value |
|------|-----------|-----------------|
| Control | **23456** | 12345 |
| Data (ucast) | **29000** | 5000 |
| RTT local | **29020+** | 19020+ |

The control port is exported via `AFXDP_CONTROL_PORT` before any binary is spawned
(see `conftest.py` and `src/common/ControlPort.hpp`).

---

## Running the Tests

### Prerequisites

- Python 3.9+, `pytest` (`pip install pytest`)
- Built binaries — tests skip gracefully if missing

### Locally (from the af_xdp root)

```bash
# Build first (echo-mode is sufficient for tests; full needs libxdp)
make echo-mode        # or: make full

# Run
pytest tests/ -v
pytest tests/ -v -k TestRTTMeasurement       # single class
pytest tests/ -v -k test_echo_to_registered   # single test
```

### Docker (recommended for CI / Apple Silicon)

The Docker harness (`dev/Dockerfile`) mirrors the AMI bake environment (Amazon
Linux 2023, xdp-tools from source, `make full`) then runs the full pytest suite.
The Makefile targets x86_64 (`-msse4.2/-mavx2/-mfma`, `rdtsc`), so build for
`linux/amd64`:

```bash
cd networking_benchmarks/af_xdp

# Build image (compiles everything — validates AMI bake parity)
docker build --platform linux/amd64 -f dev/Dockerfile -t afxdp-test .

# Run tests (default CMD)
docker run --rm --platform linux/amd64 afxdp-test

# Iterate on tests without rebuilding binaries:
docker run --rm --platform linux/amd64 -v "$PWD/tests:/src/tests" afxdp-test
```

### On the EC2 Fleet (Ansible)

`dev/ansible/run_tests.yaml` runs the same 45 tests on every provisioned fleet
node. It:

1. Installs `pytest` (if missing).
2. **Stops** `replicator.service` — the production systemd unit — because the
   test suite spawns its own echo-mode replicator on port 23456 (in code; the
   playbook comment mentions 12345, but the test fixtures override via
   `AFXDP_CONTROL_PORT`).
3. Runs `pytest tests/ -v --tb=short`.
4. **Always** restarts `replicator.service` (even on failure).
5. Fails the play if `pytest` returned non-zero.

```bash
cd networking_benchmarks/af_xdp/dev/ansible
ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml
ansible-playbook -i inventory.aws_ec2.yml run_tests.yaml --limit replicator
```

---

## Fixtures

Defined in `conftest.py` (session-scoped) and `test_integration.py`
(module/function-scoped):

| Fixture | Scope | Location | Description |
|---------|-------|----------|-------------|
| `check_binaries` | **session** (autouse) | `conftest.py` | Skips the entire suite if `replicator` binary is not found at `af_xdp/replicator` |
| `replicator_process` | **module** | `test_integration.py` | Starts `replicator --echo-mode 127.0.0.1 29000` with `AFXDP_CONTROL_PORT=23456`; SIGTERM + wait on teardown |
| `ctrl_socket` | **function** | `test_integration.py` | Fresh UDP socket (3 s timeout) for control-protocol assertions |

### Skip / Skip-Ahead Conditions

- If `af_xdp/replicator` does not exist → **session skips** (`conftest.py`).
- If individual tool binaries (`rtt`, `replicator_ctl`, `udp_send`, `mcast_send`,
  `mcast_receive`) do not exist → **those tests skip** individually via
  `pytest.skip(...)`.
- If `replicator --echo-mode` exits immediately on startup → `pytest.fail()`.

There are no custom pytest markers.

---

## Test Classes & Cases (45 tests)

### `TestBinaries` (4 tests)
Existence checks — asserts the compiled binary is present at the expected path.

| Test | Asserts |
|------|---------|
| `test_replicator_exists` | `af_xdp/replicator` file exists |
| `test_rtt_exists` | `af_xdp/rtt` file exists |
| `test_replicator_ctl_exists` | `af_xdp/replicator_ctl` file exists |
| `test_udp_send_exists` | `af_xdp/udp_send` file exists |

### `TestReplicatorLaunchOptions` (6 tests)
Container-safe argument-layer validation of the replicator binary.

| Test | Asserts |
|------|---------|
| `test_no_args_usage` | Exit ≠ 0; output contains "Usage" |
| `test_echo_mode_missing_args_usage` | `--echo-mode` with no IP/port → exit ≠ 0, "Usage" + "--echo-mode" in output |
| `test_usage_documents_launch_options` | Usage text advertises: `--echo-mode`, `zero_copy`, `--mcast`, `--ctrl`, `--producer` |
| `test_ctrl_without_producer_rejected` | `--ctrl` without `--producer` → exit ≠ 0 (invalid combo validated before XDP) |
| `test_mcast_join_not_acked_by_echo_mode` | CTRL_MCAST_JOIN (opcode 4) → NAK `0x00` (echo-mode is unicast-only) |
| `test_mcast_leave_not_acked_by_echo_mode` | CTRL_MCAST_LEAVE (opcode 5) → NAK `0x00` |

### `TestControlProtocol` (4 tests)
Happy-path control protocol on the echo-mode replicator.

| Test | Asserts |
|------|---------|
| `test_add_destination` | ADD → ACK `0x01` |
| `test_add_duplicate_destination` | Two ADDs to same dest → both ACK `0x01` (idempotent) |
| `test_list_destinations` | LIST response: `[1B count][count × (4B IP + 2B port)]`; count ≥ 1 |
| `test_remove_destination` | ADD then REMOVE → ACK `0x01` |

### `TestControlProtocolNegative` (3 tests)
Robustness / error-handling of the control port.

| Test | Asserts |
|------|---------|
| `test_remove_unknown_destination` | REMOVE for never-registered dest → NAK `0x00` |
| `test_unknown_command` | Command byte 99 → NAK `0x00` |
| `test_malformed_add_too_short` | ADD with truncated payload (1 byte only) → NAK `0x00` |

### `TestDataEcho` (5 tests)
Functional data-plane echo through the kernel-socket stand-in.

| Test | Asserts |
|------|---------|
| `test_echo_to_registered_destination` | Packet sent to DATA_PORT echoes to a registered receiver byte-for-byte |
| `test_no_echo_to_unregistered` | Unregistered receiver gets no data (socket.timeout) |
| `test_multi_destination_fanout` | Single packet echoed to all registered receivers |
| `test_remove_stops_echo` | After REMOVE, destination stops receiving (socket.timeout) |
| `test_large_payload_echo` | 1283-byte binary payload (full byte range) round-trips intact |

### `TestRTTMeasurement` (8 tests)
End-to-end invocation of the `rtt` measurement binary.

| Test | Asserts |
|------|---------|
| `test_rtt_produces_json` | Exit 0; `/tmp/rtt_results.json` written; `service_rtt_us` contains `min`, `p50`, `p99`, `max`; p50 > 0; p99 ≥ p50 |
| `test_rtt_json_schema_fields` | Top-level JSON keys: `messages`, `warmup`, `rate_mps`, `lost`, `loss_pct`, `timestamp_rx`, `timestamp_tx`; `rate_mps` == 1000; `timestamp_tx` == `"clock_realtime"` |
| `test_rtt_zero_loss_localhost` | `lost` == 0 on localhost kernel echo |
| `test_rtt_respects_warmup` | With total=500, warmup=400 → `messages` is 500 or 100 |
| `test_rtt_localhost_latency_sanity` | p50 < 5000 µs on localhost |
| `test_rtt_invalid_args` | Too few args → exit ≠ 0, "Usage" in output |
| `test_rtt_xdp_tx_requires_iface` | `--xdp-tx` without `--iface` → exit ≠ 0; output mentions "iface" or "echo-mode" |
| `test_rtt_usage_documents_flags` | Usage text advertises `--xdp-tx`, `--iface`, `--xdp-rx` |

### `TestReplicatorCtl` (3 tests)
CLI round-trip of the `replicator_ctl` admin tool.

| Test | Asserts |
|------|---------|
| `test_ctl_add_then_list` | `add` → exit 0, "successful"; `list` → exit 0, port visible in output |
| `test_ctl_usage_on_no_command` | No args → exit ≠ 0, "Usage" |
| `test_ctl_usage_documents_commands` | Usage advertises: `add`, `remove`, `list`, `mcast`, `mcast-leave` |

### `TestUdpSend` (2 tests)
CLI surface of the `udp_send` load-generation tool.

| Test | Asserts |
|------|---------|
| `test_udp_send_help` | No args → exit ≠ 0, "Usage" |
| `test_udp_send_usage_documents_iface` | Usage advertises `--iface` and "multicast" |

### `TestMcastBinaries` (2 tests)
Existence checks for multicast tools.

| Test | Asserts |
|------|---------|
| `test_mcast_send_exists` | `af_xdp/mcast_send` exists |
| `test_mcast_receive_exists` | `af_xdp/mcast_receive` exists |

### `TestMcastSendCli` (4 tests)
Argument-parsing layer of `mcast_send` (runs before any AF_XDP/root/NIC access).

| Test | Asserts |
|------|---------|
| `test_help_exits_zero` | `-h` → exit 0, "Usage" |
| `test_missing_required_dst_fails` | No args → exit ≠ 0; output contains `-D` or "required" |
| `test_unknown_option_fails` | `-Z` → exit ≠ 0, "Usage" |
| `test_help_documents_options` | `-h` advertises: `-D`, `-g`, `-p`, `-c`, `-i` |

### `TestMcastReceiveCli` (4 tests)
Argument-parsing layer of `mcast_receive` (runs before XDP attach).

| Test | Asserts |
|------|---------|
| `test_help_exits_zero` | `-h` → exit 0, "Usage" |
| `test_missing_required_iface_fails` | No args → exit ≠ 0; output contains `-I` or "required" |
| `test_unknown_option_fails` | `-Z` → exit ≠ 0, "Usage" |
| `test_help_documents_options` | `-h` advertises: `-I`, `-g`, `-p`, `-c`, `-t` |

---

## What These Tests Do NOT Cover

The following require root + AF_XDP + a real NIC (EC2 with ENA):

- **Real zero-copy unicast** (`replicator <iface> <ip> <port> [zero_copy]`) —
  validated by `deploy/ansible/run_ucast.yaml` using the `rtt` tool in `--xdp-tx` /
  `--xdp-rx` / `--xdp-txrx` modes across 1K / 10K / 100K packets.
- **Multicast-over-unicast (m2u)** datapath — validated by
  `deploy/ansible/run_mcast.yaml` using `mcast_send` and `mcast_receive` in copy /
  in-place / kernel modes across the fleet.
- **Hardware timestamping** — separate networking benchmark
  (`networking_benchmarks/hw_timestamping`).

---

## File Layout

```
tests/
├── __init__.py              # Package marker (empty)
├── conftest.py              # Session-scoped fixtures & port env export
├── test_integration.py      # All 45 test cases (11 classes)
├── README.md                # ← you are here
└── assets/
    └── test-execution-overview.svg   # Execution-path diagram
```

---

## `pytest.ini`

Located at `af_xdp/pytest.ini` (project root):

```ini
[pytest]
testpaths = tests
addopts = -v --tb=short
```

---

## License

This project is licensed under the MIT-0 License. See the top-level LICENSE file.

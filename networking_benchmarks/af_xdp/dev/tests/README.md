# dev/tests/

Integration tests for the AF_XDP benchmark (pytest). They exercise the real
`rtt`, `replicator_ctl`, `udp_send`, `mcast_send`, `mcast_receive` binaries and
the control protocol against a kernel-mode replicator — no root, no XDP/BPF — so
they run in containers / CI / macOS. The production AF_XDP datapath itself is
validated on EC2 (`run_ucast.yaml` / `run_mcast.yaml`).

Non-production ports are used (control **23456**, data **29000**, rtt-local
**29020+**) so the suite never collides with a live `replicator.service`. The
control port is exported via `AFXDP_CONTROL_PORT` (see `src/ControlPort.hpp`).

## Running

```bash
# Build first (from af_xdp/): full needs libxdp; kernel-mode runs anywhere
make full            # or: make kernel-mode

pytest dev/tests/ -v
pytest dev/tests/ -v -k TestRTTMeasurement    # one class
```

Binaries are located three parents up from the tests (the af_xdp root, where
`make` puts them). Tests skip gracefully if a binary is missing.

## Test classes (34 tests)

| Class | # | Covers |
|-------|---|--------|
| `TestBinaries` | 4 | `replicator` / `rtt` / `replicator_ctl` / `udp_send` exist |
| `TestControlProtocol` | 4 | ADD, ADD-duplicate, LIST (count+entries), REMOVE → ACK |
| `TestControlProtocolNegative` | 3 | remove-unknown, unknown-command, malformed-ADD → NAK |
| `TestDataEcho` | 5 | echo to registered, no echo to unregistered, multi-destination fan-out, remove-stops-echo, near-MTU binary payload |
| `TestRTTMeasurement` | 7 | JSON produced, schema fields, zero-loss localhost, warmup exclusion, localhost latency sanity, invalid-args, `--xdp-tx` requires `--iface` |
| `TestReplicatorCtl` | 2 | `replicator_ctl` add→list round-trip, usage on no command |
| `TestUdpSend` | 1 | `udp_send` CLI help |
| `TestMcastBinaries` | 2 | `mcast_send` / `mcast_receive` exist |
| `TestMcastSendCli` | 3 | `-h`, missing `-D`, unknown option |
| `TestMcastReceiveCli` | 3 | `-h`, missing `-I`, unknown option |

The mcast tests cover only the CLI/arg layer (the GRE datapath needs root + XDP +
a real NIC — EC2 only, via `run_mcast.yaml`).

## Fixtures (conftest.py + test module)

| Fixture | Scope | Description |
|---------|-------|-------------|
| `check_binaries` | session (autouse) | Skips the suite if the `replicator` binary is missing |
| `replicator_process` | module | Starts/stops `replicator --kernel-mode` (control port from `AFXDP_CONTROL_PORT`) |
| `ctrl_socket` | function | UDP socket for control-protocol tests |

## Requirements

- Python 3.9+, `pytest` (`pip install pytest`)
- Built binaries (tests skip gracefully if missing)

## Container testing (recommended)

The Docker harness (`dev/docker/Dockerfile`) mirrors the AMI bake (xdp-tools +
`make full`) then runs the suite. The Makefile targets x86_64, so build for
`linux/amd64` (emulated on Apple Silicon):

```bash
# from af_xdp/
docker build --platform linux/amd64 -f dev/docker/Dockerfile -t afxdp-test .
docker run  --rm --platform linux/amd64 afxdp-test            # runs pytest dev/tests/

# iterate on tests without rebuilding binaries:
docker run --rm --platform linux/amd64 -v "$PWD/dev/tests:/src/dev/tests" afxdp-test
```

All 34 tests pass in an AL2023 container.

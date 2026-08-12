# tests/

Integration tests for the AF_XDP benchmark (pytest). They exercise the real
`rtt`, `replicator_ctl`, `udp_send`, `mcast_send`, `mcast_receive` binaries and
the control protocol against an echo-mode replicator — no root, no XDP/BPF — so
they run in containers / CI / macOS. The production AF_XDP datapath itself is
validated on EC2 (`run_ucast.yaml` / `run_mcast.yaml`).

Non-production ports are used (control **23456**, data **29000**, rtt-local
**29020+**) so the suite never collides with a live `replicator.service`. The
control port is exported via `AFXDP_CONTROL_PORT` (see `src/common/ControlPort.hpp`).

## Running

```bash
# Build first (from af_xdp/): full needs libxdp; echo-mode runs anywhere
make full            # or: make echo-mode

pytest tests/ -v
pytest tests/ -v -k TestRTTMeasurement    # one class
```

Binaries are located two parents up from the tests (the af_xdp root, where
`make` puts them). Tests skip gracefully if a binary is missing.

## Test classes (45 tests)

| Class | # | Covers |
|-------|---|--------|
| `TestBinaries` | 4 | `replicator` / `rtt` / `replicator_ctl` / `udp_send` exist |
| `TestReplicatorLaunchOptions` | 6 | usage + arg-count rejection; usage documents echo-mode/ucast `zero_copy`/`--mcast`/`--ctrl`/`--producer`; `--ctrl` without `--producer` rejected; MCAST_JOIN/LEAVE NAK'd by the echo-mode (unicast) backend |
| `TestControlProtocol` | 4 | ADD, ADD-duplicate, LIST (count+entries), REMOVE → ACK |
| `TestControlProtocolNegative` | 3 | remove-unknown, unknown-command, malformed-ADD → NAK |
| `TestDataEcho` | 5 | echo to registered, no echo to unregistered, multi-destination fan-out, remove-stops-echo, near-MTU binary payload |
| `TestRTTMeasurement` | 8 | JSON produced, schema fields, zero-loss localhost, warmup exclusion, localhost latency sanity, invalid-args, `--xdp-tx` requires `--iface`, usage documents `--xdp-tx`/`--iface`/`--xdp-rx` |
| `TestReplicatorCtl` | 3 | add→list round-trip, usage on no command, usage documents `add`/`remove`/`list`/`mcast`/`mcast-leave` |
| `TestUdpSend` | 2 | CLI help, usage documents `--iface`/multicast |
| `TestMcastBinaries` | 2 | `mcast_send` / `mcast_receive` exist |
| `TestMcastSendCli` | 4 | `-h`, missing `-D`, unknown option, help documents `-D`/`-g`/`-p`/`-c`/`-i` |
| `TestMcastReceiveCli` | 4 | `-h`, missing `-I`, unknown option, help documents `-I`/`-g`/`-p`/`-c`/`-t` |

The mcast tools' launch tests cover only the CLI/arg layer (the m2u AF_XDP
datapath needs root + XDP + a real NIC — EC2 only, via `run_mcast.yaml`); ucast
and mcast zero-copy on/off launches are likewise validated on EC2
(`run_ucast.yaml` / `run_mcast.yaml`).

## Fixtures (conftest.py + test module)

| Fixture | Scope | Description |
|---------|-------|-------------|
| `check_binaries` | session (autouse) | Skips the suite if the `replicator` binary is missing |
| `replicator_process` | module | Starts/stops `replicator --echo-mode` (control port from `AFXDP_CONTROL_PORT`) |
| `ctrl_socket` | function | UDP socket for control-protocol tests |

## Requirements

- Python 3.9+, `pytest` (`pip install pytest`)
- Built binaries (tests skip gracefully if missing)

## Container testing (recommended)

The Docker harness (`dev/Dockerfile`) mirrors the AMI bake (xdp-tools +
`make full`) then runs the suite. The Makefile targets x86_64, so build for
`linux/amd64` (emulated on Apple Silicon):

```bash
# from af_xdp/
docker build --platform linux/amd64 -f dev/Dockerfile -t afxdp-test .
docker run  --rm --platform linux/amd64 afxdp-test            # runs pytest tests/

# iterate on tests without rebuilding binaries:
docker run --rm --platform linux/amd64 -v "$PWD/tests:/src/tests" afxdp-test
```

All 45 tests pass in an AL2023 container.

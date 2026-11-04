# Native sender and receiver

C++ multicast sender and receiver. They share one wire format.

| File | Purpose |
|---|---|
| `mcast_send.cpp` | Paced multicast sender. |
| `mcast_recv.cpp` | Receiver. Hardware RX stamp is primary; the kernel software stamp is kept for divergence |
| `lat_stats.hpp` | Bounded-memory statistics (histograms, sequence tracking, outliers). Portable |
| `tests/stats_test.cpp` | Unit tests for the statistics. Run anywhere |
| `tests/docker/` | Container harness: both implementations under several load profiles |

## Build

```bash
make                 # mcast_send, mcast_recv  -> build/
make stats_test      # portable unit tests
make synth           # mcast_recv_synth: TEST ONLY, see below
```

No `-march` flag: the instance gate allows x86-64 and Graviton, and the same source
builds for both. `mcast_recv` is Linux only. Needs `g++` (the bootstrap already installs it).

## Usage

```
mcast_send <group> <port> <rate_pps> <duration_secs> <src_ip> [payload_bytes]
mcast_recv <group> <port> <timeout_secs> [iface]
```

Positional arguments as below. `payload_bytes` is optional (12 to 1472).
`rate_pps` 0 sends unpaced.

### Sender

- Pacing is against an absolute deadline `t0 + i * interval`, so overshoot is absorbed
  rather than accumulated. The wait is a spin, which keeps the send path hot. A stall of
  more than 100 ms shifts the schedule instead of releasing a burst.
- `tx_ns` is taken immediately before `sendto()`. ENA has no hardware TX stamp, so this
  remains a software stamp.
- Prints one JSON summary line: sent, errors, achieved rate and percent, worst lateness,
  CPU time.

### Receiver

- Requires a real hardware RX stamp. If `SIOCSHWTSTAMP` fails, or a packet arrives without
  one, it prints an error JSON and exits 1. There is no software-only mode.
- Reads the software stamp from the same message and reports `div_*`: software minus
  hardware, per packet. That is receive-path delay plus any offset between `CLOCK_REALTIME`
  and the PHC. A large or unstable value means the clock domains disagree or the receive
  path is stalling. A spike that appears in `sw_*` but not in the hardware numbers is a
  receiver-side stall.
- Output is exactly **one flat JSON line** on stdout. The orchestrator captures
  stdout and stderr into one file and `save_report.py` extracts the first object with a
  non-greedy brace regex, so nested objects would break it. Everything the existing report
  reads is present with the same meaning. Percentiles come from a 1/1024-resolution
  histogram (error under 0.05%), not a sorted array.
- Extra fields: `p999_*`, `sw_*`, `div_*`, `negative_*`, `duplicate_count`,
  `out_of_order_count`, `socket_drops` (`SO_RXQ_OVFL`), `udp_rcvbuf_errors_delta`,
  `rcvbuf_bytes_effective`, `first100_max_latency_us`, achieved receive rate, CPU time.
- `total_received` counts **unique** sequence numbers, so duplicates no longer offset
  loss.
- Writes `/tmp/mcast_recv.ready` once bound, joined and stamping (removed on exit), and
  `/tmp/mcast_recv_detail.json` with mergeable histograms, the 16 worst packets by
  sequence number, and per-second maxima.
- Enabling hardware stamping briefly disrupts networking (ENA README). It happens before
  the join and readiness. On exit, and on SIGINT or SIGTERM, the NIC is restored.

## Test-only build

Containers have no hardware stamps. `make synth` builds `mcast_recv_synth`, which
tolerates a failed `SIOCSHWTSTAMP` and derives `hw = sw - 700 ns`. It exists so the
rest of the pipeline can be tested. **Never deploy it.** The shipped `mcast_recv` has
no such path, and its refusal to run without hardware stamps is itself a harness check.

## Container harness

```bash
docker build -t tgw-native-test tests/docker
python3 tests/docker/harness.py                     # all profiles
python3 tests/docker/harness.py --only netem,slow_rx
```

Results go to `/tmp/native_results/<timestamp>/summary.md`. `TGW_TEST_PLATFORM=linux/amd64`
runs the containers under x86-64 emulation (correctness only; timing is meaningless).

What it can and cannot tell you:

- It validates counts, accounting, parsing, lifecycle and capacity, with ground truth from
  the kernel (interface counters, `UdpRcvbufErrors`, `tc` statistics).
- It cannot validate the real hardware stamp, ENA behavior, x86 hardware timing, or any real
  latency. Container latencies are an artifact of a shared VM.
- The Docker Desktop VM clock steps by 0.1-0.7 ms, in both directions, every 10-30 s even when idle (measured over
  150 s). One run produced a single -4.6 ms latency out of 463k packets, and the receiver
  reported the identical value, so the packet itself carried it. The harness tolerates up to 1 negative sample per
  100,000 and reports the count; a parsing or offset bug would produce them in bulk.
- The container's `net.core.rmem_max` is 212,992, so the requested 16 MB receive buffer is
  clamped to about 0.4 MB. Drops in the high-rate profiles are partly that. The effective
  size is reported as `rcvbuf_bytes_effective`.

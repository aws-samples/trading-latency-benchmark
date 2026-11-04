# host: programs that run on the benchmark instances

This directory holds what runs on the instances: `preflight.sh` (clock and NIC checks) and the C++
multicast sender/receiver pair in `cpp/` (`mcast_send.cpp`, `mcast_recv.cpp`, `lat_stats.hpp`; Linux only).

The **sender** emits sequenced UDP datagrams to a multicast group, each carrying the sender's
`CLOCK_REALTIME` stamp. The **receiver** joins the group and measures one-way latency from the NIC
hardware receive stamp.

Wire format: bytes 0-3 sequence number (uint32), bytes 4-11 send time in ns since the epoch (uint64),
both in network byte order, optionally followed by zero padding. A sequence number of `0xFFFFFFFF`
marks the end of the stream and carries the number of data packets sent in the time field.

## Sender

- Absolute-deadline pacing (`t0 + i/rate`), so delays are absorbed instead of accumulating.
- Reports the achieved rate against the requested rate, the worst send lateness and the resync count;
  the orchestrator flags shortfalls.
- Reports the time spent inside `sendto` per packet.
- Stamps immediately before `sendto`, using a preallocated buffer.
- About 790k pps from one thread on `m8a.xlarge`. Optional padding up to a chosen payload size.
- `MCAST_SRC_PORT` sets one source port or a `LO-HI` range.

## Receiver

- `recvmmsg` batches with constant-size histograms, so memory does not grow with rate or duration.
- Writes mergeable histograms, so the orchestrator computes pooled percentiles across subscribers.
- Keeps both the software and hardware stamps and reports the gap, which flags clock-domain problems
  and receiver stalls.
- Drops the first 2 s from latency statistics (`MCAST_WARMUP_SECS`); loss still counts them.
- Sequence bitmap: duplicates, out-of-order and short packets are counted separately.
- Reads `SO_RXQ_OVFL` and the UDP receive-buffer error deltas, so a full socket buffer is separated
  from network loss.
- Counts negative latencies without clamping and reports them as a failing finding.
- Writes `/tmp/mcast_recv.ready` after joining the group and enabling stamping.
- Refuses to run without a real hardware stamp.
- Detail file with outliers and per-second maxima, CPU time, effective receive-buffer size.

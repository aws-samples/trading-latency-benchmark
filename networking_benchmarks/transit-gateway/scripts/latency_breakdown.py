#!/usr/bin/env python3
"""
latency_breakdown.py — Decompose multicast latency into measurable segments.

Produces a waterfall/flame-graph-style breakdown showing where time is spent
in the end-to-end multicast path:

  [Sender App] → [Sender Kernel/NIC] → [TGW] → [Receiver NIC/Kernel] → [Receiver App]

Segments measured:
  1. send_syscall_us    — time inside sendto() call (app → kernel handoff)
  2. network_transit_us — wire + TGW + wire (kernel TX to kernel RX)
  3. recv_syscall_us    — time from kernel RX to app (recvfrom return)
  4. total_us           — end-to-end (sender app timestamp to receiver app timestamp)

The sender embeds two timestamps per packet:
  - t_before_send: just before sendto()
  - t_after_send:  just after sendto()

The receiver records:
  - t_recv: from recvfrom() return

With PTP-synced clocks:
  send_syscall  = t_after_send - t_before_send  (same host, no clock sync needed)
  total         = t_recv - t_before_send         (cross-host, needs PTP)
  network+recv  = t_recv - t_after_send          (cross-host, needs PTP)

Usage:
  # On publisher (via SSM):
  python3 /tmp/latency_breakdown.py send <group> <port> <rate> <duration> <src_ip>

  # On subscriber (via SSM):
  python3 /tmp/latency_breakdown.py recv <group> <port> <timeout>

  # Analyze (locally):
  python3 scripts/latency_breakdown.py analyze --sender sender.json --receiver receiver.json
"""
import socket, struct, time, json, sys, os, statistics, argparse


def cmd_send(group, port, rate, duration, src_ip):
    """Sender: embed pre/post sendto timestamps in each packet."""
    try:
        os.sched_setaffinity(0, {0})
    except (AttributeError, OSError):
        pass

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 32)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(src_ip))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)

    _clock = time.clock_gettime
    _CK = time.CLOCK_REALTIME
    _sendto = s.sendto
    dest = (group, port)
    interval = 1.0 / rate

    records = []
    seq = 0
    end_time = _clock(_CK) + duration

    while _clock(_CK) < end_time:
        t_before = _clock(_CK)
        # Pack: seq(4B) + t_before(8B double) — then t_after appended after send
        payload = struct.pack("!Id", seq, t_before)
        _sendto(payload, dest)
        t_after = _clock(_CK)
        records.append((seq, t_before, t_after))
        seq += 1
        # Busy-wait for interval
        target = t_before + interval
        while _clock(_CK) < target:
            pass

    # Send END markers
    for _ in range(3):
        _sendto(b"END", dest)
        time.sleep(0.1)

    # Write full records to file for later analysis
    full_output = {
        "role": "sender",
        "packets": seq,
        "records": [
            {"seq": r[0], "t_before_send": r[1], "t_after_send": r[2],
             "send_syscall_us": (r[2] - r[1]) * 1e6}
            for r in records
        ],
    }
    with open("/tmp/breakdown_send_full.json", "w") as f:
        json.dump(full_output, f)

    # Output summary only to stdout (SSM has 24KB output limit)
    output = {
        "role": "sender",
        "packets": seq,
    }
    # Summary
    syscalls = [r[2] - r[1] for r in records]
    syscalls_us = [x * 1e6 for x in syscalls]
    syscalls_us.sort()
    n = len(syscalls_us)
    if n > 0:
        output["send_syscall_stats"] = {
            "min_us": round(syscalls_us[0], 3),
            "median_us": round(syscalls_us[n // 2], 3),
            "mean_us": round(statistics.mean(syscalls_us), 3),
            "p95_us": round(syscalls_us[int(n * 0.95)], 3),
            "p99_us": round(syscalls_us[int(n * 0.99)], 3),
            "max_us": round(syscalls_us[-1], 3),
        }

    print(json.dumps(output))


def cmd_recv(group, port, timeout):
    """Receiver: record receive timestamp for each packet."""
    try:
        os.sched_setaffinity(0, {1})
    except (AttributeError, OSError):
        pass

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
    try:
        s.setsockopt(socket.SOL_SOCKET, 46, 50)  # SO_BUSY_POLL
    except OSError:
        pass

    s.bind(("", port))
    mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton("0.0.0.0"))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.settimeout(timeout)

    _clock = time.clock_gettime
    _CK = time.CLOCK_REALTIME

    records = []
    try:
        while True:
            data, _ = s.recvfrom(128)
            t_recv = _clock(_CK)
            if data == b"END":
                break
            if len(data) >= 12:
                seq, t_before_send = struct.unpack("!Id", data[:12])
                records.append((seq, t_before_send, t_recv))
    except socket.timeout:
        pass

    # Compute per-packet breakdown
    results = []
    for seq, t_before_send, t_recv in records:
        total_us = (t_recv - t_before_send) * 1e6
        results.append({
            "seq": seq,
            "t_before_send": t_before_send,
            "t_recv": t_recv,
            "total_us": total_us,
        })

    # Write full records to file
    full_output = {
        "role": "receiver",
        "packets": len(results),
        "records": results,
    }
    with open("/tmp/breakdown_recv_full.json", "w") as f:
        json.dump(full_output, f)

    # Output summary only to stdout
    output = {
        "role": "receiver",
        "packets": len(results),
    }

    # Summary stats
    if results:
        totals = sorted(r["total_us"] for r in results)
        n = len(totals)
        output["total_latency_stats"] = {
            "min_us": round(totals[0], 3),
            "median_us": round(totals[n // 2], 3),
            "mean_us": round(statistics.mean(totals), 3),
            "p95_us": round(totals[int(n * 0.95)], 3),
            "p99_us": round(totals[int(n * 0.99)], 3),
            "max_us": round(totals[-1], 3),
        }

    print(json.dumps(output))


def cmd_analyze(sender_file, receiver_file):
    """Combine sender + receiver data to produce a full latency breakdown."""
    with open(sender_file) as f:
        sender = json.load(f)
    with open(receiver_file) as f:
        receiver = json.load(f)

    # Index sender records by seq
    send_by_seq = {}
    for r in sender["records"]:
        send_by_seq[r["seq"]] = r

    # Match and compute breakdown
    breakdowns = []
    for r in receiver["records"]:
        seq = r["seq"]
        if seq not in send_by_seq:
            continue
        sr = send_by_seq[seq]

        send_syscall = sr["send_syscall_us"]
        total = r["total_us"]
        # network_plus_recv = total - send_syscall
        network_plus_recv = (r["t_recv"] - sr["t_after_send"]) * 1e6

        breakdowns.append({
            "seq": seq,
            "send_syscall_us": round(send_syscall, 3),
            "network_transit_us": round(network_plus_recv, 3),
            "total_us": round(total, 3),
        })

    if not breakdowns:
        print("No matched packets found.")
        return

    # Aggregate stats per segment
    def stats(values):
        values.sort()
        n = len(values)
        return {
            "min": round(values[0], 3),
            "median": round(values[n // 2], 3),
            "mean": round(statistics.mean(values), 3),
            "p95": round(values[int(n * 0.95)], 3),
            "p99": round(values[int(n * 0.99)], 3),
            "max": round(values[-1], 3),
        }

    send_syscalls = [b["send_syscall_us"] for b in breakdowns]
    network_transits = [b["network_transit_us"] for b in breakdowns]
    totals = [b["total_us"] for b in breakdowns]

    report = {
        "matched_packets": len(breakdowns),
        "segments": {
            "send_syscall_us": stats(send_syscalls),
            "network_transit_us": stats(network_transits),
            "total_us": stats(totals),
        },
    }

    # Waterfall at median
    med_send = report["segments"]["send_syscall_us"]["median"]
    med_network = report["segments"]["network_transit_us"]["median"]
    med_total = report["segments"]["total_us"]["median"]

    report["waterfall_median"] = {
        "send_syscall_us": med_send,
        "network_transit_us": med_network,
        "total_us": med_total,
        "pct_send_syscall": round(med_send / med_total * 100, 1),
        "pct_network_transit": round(med_network / med_total * 100, 1),
    }

    # ASCII flame graph
    bar_width = 60
    pct_send = med_send / med_total
    pct_net = med_network / med_total
    bar_send = int(bar_width * pct_send)
    bar_net = bar_width - bar_send

    print("=" * 70)
    print("LATENCY BREAKDOWN (median values)")
    print("=" * 70)
    print(f"Total one-way: {med_total:.1f} μs")
    print()
    print(f"  send_syscall    [{med_send:7.1f} μs] {'█' * max(bar_send, 1)}{'░' * bar_net} {pct_send*100:.1f}%")
    print(f"  network+recv    [{med_network:7.1f} μs] {'░' * bar_send}{'█' * max(bar_net, 1)} {pct_net*100:.1f}%")
    print()
    print("Segment stats (μs):")
    print(f"  {'Segment':<20} {'Min':>8} {'Median':>8} {'Mean':>8} {'P95':>8} {'P99':>8} {'Max':>8}")
    print(f"  {'-'*20} {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    for name, s in report["segments"].items():
        label = name.replace("_us", "")
        print(f"  {label:<20} {s['min']:>8.1f} {s['median']:>8.1f} {s['mean']:>8.1f} {s['p95']:>8.1f} {s['p99']:>8.1f} {s['max']:>8.1f}")
    print()
    print("Note: network_transit includes TGW processing + wire + receiver kernel/app overhead.")
    print("      send_syscall is measured on the sender host (no clock sync needed).")
    print("=" * 70)

    # Also print JSON for programmatic use
    print()
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd")

    p_send = sub.add_parser("send")
    p_send.add_argument("group")
    p_send.add_argument("port", type=int)
    p_send.add_argument("rate", type=int)
    p_send.add_argument("duration", type=int)
    p_send.add_argument("src_ip")

    p_recv = sub.add_parser("recv")
    p_recv.add_argument("group")
    p_recv.add_argument("port", type=int)
    p_recv.add_argument("timeout", type=int)

    p_analyze = sub.add_parser("analyze")
    p_analyze.add_argument("--sender", required=True)
    p_analyze.add_argument("--receiver", required=True)

    args = parser.parse_args()
    if args.cmd == "send":
        cmd_send(args.group, args.port, args.rate, args.duration, args.src_ip)
    elif args.cmd == "recv":
        cmd_recv(args.group, args.port, args.timeout)
    elif args.cmd == "analyze":
        cmd_analyze(args.sender, args.receiver)
    else:
        parser.print_help()

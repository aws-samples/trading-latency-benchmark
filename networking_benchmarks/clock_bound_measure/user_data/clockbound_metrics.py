#!/usr/bin/env python3
# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: MIT-0
"""Publish ClockBound error bound and clock status to CloudWatch.

Reads the ClockBound protocol v3 shared memory segment at 1 Hz and publishes one
statistic set per metric per minute. A minute with no valid samples publishes
nothing, so gaps mean "bound unknown" rather than a fabricated value.

Run with --selftest to check the segment parser without an instance.
"""
import mmap
import struct
import sys
import time
import urllib.request

SHM_PATH = "/var/run/clockbound/shm1"
NAMESPACE = "ClockBoundMeasure"
# Two 32-bit words written little-endian, so the wire bytes look reversed.
MAGIC = struct.pack("<II", 0x414D5A4E, 0x43420200)
SEGMENT_SIZE = 104
SAMPLE_INTERVAL_S = 1
PUBLISH_INTERVAL_S = 60

# Protocol v3 offsets, little endian (clock-bound docs/protocol.md).
OFF_GENERATION = 14
OFF_VOID_AFTER_SEC = 40
OFF_VOID_AFTER_NSEC = 48
OFF_BOUND_NS = 72
OFF_CLOCK_STATUS = 92

# Void-After is stamped against the coarse monotonic clock, not the wall clock.
MONOTONIC_COARSE = getattr(time, "CLOCK_MONOTONIC_COARSE", time.CLOCK_MONOTONIC)


def parse(buf, now_s):
    """Return (bound_us, clock_status), or None if the segment is unusable.

    buf must be the live mapping: the generation counter is re-read afterwards to
    reject a read torn by a concurrent daemon write.
    """
    if len(buf) < SEGMENT_SIZE or buf[0:8] != MAGIC:
        return None
    generation = struct.unpack_from("<H", buf, OFF_GENERATION)[0]
    if generation == 0 or generation % 2:
        return None  # 0 = uninitialised, odd = daemon mid-write
    void_after = (
        struct.unpack_from("<q", buf, OFF_VOID_AFTER_SEC)[0]
        + struct.unpack_from("<q", buf, OFF_VOID_AFTER_NSEC)[0] / 1e9
    )
    if now_s > void_after:
        return None
    bound_ns = struct.unpack_from("<q", buf, OFF_BOUND_NS)[0]
    if bound_ns < 0:
        return None  # signed field, but a negative bound is incoherent
    status = struct.unpack_from("<i", buf, OFF_CLOCK_STATUS)[0]
    if struct.unpack_from("<H", buf, OFF_GENERATION)[0] != generation:
        return None
    return bound_ns / 1000.0, status


def read_shm(path=SHM_PATH):
    try:
        with open(path, "rb") as f, mmap.mmap(f.fileno(), SEGMENT_SIZE, prot=mmap.PROT_READ) as m:
            return parse(m, time.clock_gettime(MONOTONIC_COARSE))
    except (OSError, ValueError):
        return None


def imds(path):
    token = (
        urllib.request.urlopen(
            urllib.request.Request(
                "http://169.254.169.254/latest/api/token",
                method="PUT",
                headers={"X-aws-ec2-metadata-token-ttl-seconds": "21600"},
            ),
            timeout=5,
        )
        .read()
        .decode()
    )
    return (
        urllib.request.urlopen(
            urllib.request.Request(
                "http://169.254.169.254/latest/meta-data/" + path,
                headers={"X-aws-ec2-metadata-token": token},
            ),
            timeout=5,
        )
        .read()
        .decode()
    )


def statistic_set(values):
    return {
        "Minimum": min(values),
        "Maximum": max(values),
        "Sum": sum(values),
        "SampleCount": len(values),
    }


def publish(cloudwatch, instance_id, bounds, statuses):
    dimensions = [{"Name": "InstanceId", "Value": instance_id}]
    metric_data = [
        {
            "MetricName": "ClockErrorBound",
            "Dimensions": dimensions,
            "StatisticValues": statistic_set(bounds),
            "Unit": "Microseconds",
        },
        {
            "MetricName": "ClockStatus",
            "Dimensions": dimensions,
            "StatisticValues": statistic_set(statuses),
            "Unit": "None",
        },
    ]
    try:
        cloudwatch.put_metric_data(Namespace=NAMESPACE, MetricData=metric_data)
    except Exception as exc:
        # Broad catch: a transient throttle must not kill the daemon.
        print(f"put_metric_data failed: {exc}", file=sys.stderr, flush=True)


def main():
    # Imported here so --selftest runs without boto3 installed.
    import boto3

    instance_id = imds("instance-id")
    cloudwatch = boto3.client("cloudwatch")
    while True:
        deadline = time.monotonic() + PUBLISH_INTERVAL_S
        bounds, statuses = [], []
        while time.monotonic() < deadline:
            sample = read_shm()
            if sample is not None:
                bounds.append(sample[0])
                statuses.append(sample[1])
            time.sleep(SAMPLE_INTERVAL_S)
        if bounds:
            publish(cloudwatch, instance_id, bounds, statuses)
        else:
            print("no valid ClockBound samples this window", file=sys.stderr, flush=True)


def selftest():
    def segment(gen=2, bound_ns=63000, status=1, void_after=10.0, magic=MAGIC):
        buf = bytearray(SEGMENT_SIZE)
        buf[0:8] = magic
        struct.pack_into("<H", buf, OFF_GENERATION, gen)
        struct.pack_into("<q", buf, OFF_VOID_AFTER_SEC, int(void_after))
        struct.pack_into("<q", buf, OFF_VOID_AFTER_NSEC, int(void_after % 1 * 1e9))
        struct.pack_into("<q", buf, OFF_BOUND_NS, bound_ns)
        struct.pack_into("<i", buf, OFF_CLOCK_STATUS, status)
        return buf

    # Pinned to live-instance bytes; the fixtures below reuse MAGIC, so this is
    # the only check that catches a byte-order regression.
    assert MAGIC == bytes.fromhex("4e5a4d4100024243"), "magic byte order"
    assert parse(segment(), 5.0) == (63.0, 1), "ns must convert to us"
    assert parse(segment(status=3), 5.0) == (63.0, 3), "Disrupted must pass through"
    assert parse(segment(gen=3), 5.0) is None, "odd generation is a partial write"
    assert parse(segment(gen=0), 5.0) is None, "generation 0 is uninitialised"
    assert parse(segment(void_after=4.0), 5.0) is None, "expired bound is unusable"
    assert parse(segment(bound_ns=-1), 5.0) is None, "negative bound is incoherent"
    assert parse(segment(magic=b"XXXXXXXX"), 5.0) is None, "wrong magic"
    assert parse(bytearray(50), 5.0) is None, "truncated segment"
    assert statistic_set([2.0, 1.0, 3.0]) == {
        "Minimum": 1.0,
        "Maximum": 3.0,
        "Sum": 6.0,
        "SampleCount": 3,
    }
    print("selftest OK")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    else:
        main()

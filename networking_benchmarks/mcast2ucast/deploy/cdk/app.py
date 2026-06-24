#!/usr/bin/env python3
import sys

import aws_cdk as cdk

from stack import Mcast2UcastBenchStack, StackConfig


REQUIRED_KEYS = ["topology", "senderInstanceType", "receiverInstanceType", "amiId", "keyPairName"]
VALID_TOPOLOGIES = {"cpg", "same-az", "multi-az", "spread-az"}


def _ctx(app: cdk.App, key: str) -> str | None:
    val = app.node.try_get_context(key)
    return str(val) if val is not None else None


def _require(app: cdk.App, key: str) -> str:
    val = _ctx(app, key)
    if val is None:
        sys.stderr.write(f"ERROR: missing required CDK context key: {key}\n")
        sys.stderr.write("Pass via --context key=value (e.g. --context topology=cpg)\n")
        sys.exit(2)
    return val


def _ctx_int(app: cdk.App, key: str, default: int) -> int:
    v = _ctx(app, key)
    if v is None:
        return default
    try:
        return int(v)
    except ValueError:
        sys.stderr.write(f"ERROR: --context {key} must be a positive integer, got {v!r}\n")
        sys.exit(2)


def build_config(app: cdk.App) -> StackConfig:
    # Batch-check upfront so the error lists ALL missing keys at once.
    missing = [k for k in REQUIRED_KEYS if _ctx(app, k) is None]
    if missing:
        sys.stderr.write(f"ERROR: missing required CDK context keys: {missing}\n")
        sys.stderr.write("Pass via --context key=value (e.g. --context topology=cpg)\n")
        sys.exit(2)

    # _require() below narrows str|None -> str. Its error path is unreachable
    # here (the batch check already exited) but keeps the helper safe to
    # call from elsewhere.
    topology = _require(app, "topology")
    if topology not in VALID_TOPOLOGIES:
        sys.stderr.write(f"ERROR: topology must be one of {sorted(VALID_TOPOLOGIES)}, got {topology!r}\n")
        sys.exit(2)

    region = _ctx(app, "region") or "us-east-1"

    sender_az: str | None
    receiver_az: str | None
    receiver_azs: list[str] | None
    single_az: str | None
    if topology == "multi-az":
        sender_az = _ctx(app, "senderAz") or f"{region}a"
        receiver_az = _ctx(app, "receiverAz") or f"{region}b"
        if sender_az == receiver_az:
            sys.stderr.write(f"ERROR: multi-az topology requires senderAz != receiverAz, got {sender_az}\n")
            sys.exit(2)
        single_az = None
        receiver_azs = None
    elif topology == "spread-az":
        sender_az = _ctx(app, "senderAz") or f"{region}a"
        raw_receiver_azs = _ctx(app, "receiverAzs") or f"{region}a,{region}b,{region}c"
        receiver_azs = [az.strip() for az in raw_receiver_azs.split(",") if az.strip()]
        if not receiver_azs:
            sys.stderr.write("ERROR: spread-az topology requires at least one AZ in receiverAzs\n")
            sys.exit(2)
        if len(receiver_azs) > 4:
            sys.stderr.write(f"ERROR: spread-az topology supports at most 4 receiver AZs, got {len(receiver_azs)}\n")
            sys.exit(2)
        receiver_az = None
        single_az = None
    else:
        single_az = _ctx(app, "singleAz") or f"{region}a"
        sender_az = None
        receiver_az = None
        receiver_azs = None

    num_hosts = _ctx_int(app, "numSubscriberHosts", 1)
    num_daemons = _ctx_int(app, "numSubscriberDaemonsPerHost", 1)
    if not (1 <= num_hosts <= 32):
        sys.stderr.write(f"ERROR: numSubscriberHosts must be 1..32, got {num_hosts}\n")
        sys.exit(2)
    if not (1 <= num_daemons <= 16):
        sys.stderr.write(f"ERROR: numSubscriberDaemonsPerHost must be 1..16, got {num_daemons}\n")
        sys.exit(2)
    if num_hosts * num_daemons > 128:
        sys.stderr.write(f"ERROR: H*D must be <= 128, got {num_hosts}*{num_daemons}={num_hosts*num_daemons}\n")
        sys.exit(2)

    return StackConfig(
        topology=topology,
        sender_instance_type=_require(app, "senderInstanceType"),
        receiver_instance_type=_require(app, "receiverInstanceType"),
        ami_id=_require(app, "amiId"),
        key_pair_name=_require(app, "keyPairName"),
        region=region,
        single_az=single_az,
        sender_az=sender_az,
        receiver_az=receiver_az,
        receiver_azs=receiver_azs,
        num_subscriber_hosts=num_hosts,
        num_subscriber_daemons_per_host=num_daemons,
    )


def main() -> None:
    app = cdk.App()
    config = build_config(app)
    Mcast2UcastBenchStack(
        app,
        "Mcast2UcastBenchStack",
        config=config,
        env=cdk.Environment(region=config.region),
    )
    app.synth()


if __name__ == "__main__":
    main()

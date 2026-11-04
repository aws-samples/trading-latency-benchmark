#!/usr/bin/env python3
"""A stand-in for the `aws` CLI that answers the calls the orchestrator makes.

It does not run the instance-side commands. It recognises each by a stable token
and answers from a scenario file, and it records every SSM command so a test can
assert on what the orchestrator asked the instances to do.

Environment:
  FAKE_AWS_SCENARIO  path to the scenario JSON
  FAKE_AWS_STATE     directory for the call log, counters and the fake S3 bucket
"""
import json
import os
import re
import fcntl
import hashlib
import base64
import shutil
import sys

STATE = os.environ["FAKE_AWS_STATE"]
SC = json.load(open(os.environ["FAKE_AWS_SCENARIO"]))
BUCKET_DIR = os.path.join(STATE, "bucket")


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a else default


def counter(key):
    """Increment and return a named counter; the lock keeps parallel invocations from sharing a number."""
    path = os.path.join(STATE, "count." + re.sub(r"\W", "_", key))
    with open(os.path.join(STATE, "lock"), "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        n = int(open(path).read()) + 1 if os.path.exists(path) else 1
        open(path, "w").write(str(n))
    return n
def out(text):
    sys.stdout.write(text if text.endswith("\n") or not text else text + "\n")


def per_instance(table, inst, default):
    return SC.get(table, {}).get(inst, default)


def respond(inst, cmd):
    """Output of one SSM command on one instance."""
    if "section ptp;" in cmd:   # orchestrator check: raw PTP and clock facts
        return per_instance("check", inst, "")
    if "echo SSH=OK" in cmd:
        return "SSH=OK"
    if "ENV=READY" in cmd:
        state = per_instance("native", inst, "ready")
        lines = ["ENV=" + per_instance("env", inst, "READY"), "NATIVE=" + state]
        if state == "ready":
            lines.append("NATIVE_BUILD=commit=%s repo=r ref=main" % per_instance("commit", inst, "abc123"))
        lines.append("BOOT_ID=" + per_instance("boot_ids", inst, "boot-1"))
        lines += ["IID=" + SC.get("iid_override", {}).get(inst, inst), "AZ=" + SC["az"].get(inst, "us-east-1a"), "PRIVATE_IP=" + SC["ips"].get(inst, "10.0.0.9"),
                  "AMI=ami-0123456789abcdef0", "ITYPE=" + SC.get("instance_type", "m7i.large")]
        return "\n".join(lines)
    if "benchmark-native-build.log" in cmd:
        return SC.get("build_log", "make: *** [mcast_recv] Error 1")
    if "base64 -d" in cmd:   # one helper file installed on the instance; remember its hash
        m = re.search(r"echo '([A-Za-z0-9+/=]+)' \| base64 -d > \S+/(\S+)", cmd)
        path = os.path.join(STATE, "files", inst + ".json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        files = json.load(open(path)) if os.path.exists(path) else {}
        files[m.group(2)] = hashlib.sha256(base64.b64decode(m.group(1))).hexdigest()
        json.dump(files, open(path, "w"))
        return "UPLOADED"
    if "preflight.sh --static" in cmd:   # static checks and the context, one command
        ctx = per_instance("context", inst, "kernel=6.1.0\ncpu_model=EPYC\ncpuidle_states=POLL:0us C1:1us C2:800us")
        return ("=====STATIC=====\n" + per_instance("static", inst, "phc_enable=1\nPASS")
                + "\n=====CONTEXT=====\n" + ctx + "\nchrony_system_time_us=0.5")
    if "preflight.sh --gate" in cmd:   # boot id, helper hashes, then the per-run clock gate
        path = os.path.join(STATE, "files", inst + ".json")
        files = json.load(open(path)) if os.path.exists(path) else {}
        head = ["BOOT_ID=" + per_instance("boot_ids", inst, "boot-1")]
        head += ["SHA_%s=%s" % (k, v) for k, v in files.items()]
        return "\n".join(head + ["=====GATE=====", per_instance("preflight", inst, "system_time_us=0.1\nPASS")])
    if "=====RESULT=====" in cmd:   # one command: wait for the receiver, result, detail upload, clock
        res = per_instance("results", inst, "")
        if isinstance(res, list):  # one entry per collection, the last repeats
            n = counter("result." + inst)
            res = res[min(n, len(res)) - 1]
        if "DETAIL=SKIPPED" in cmd:
            detail = "DETAIL=SKIPPED"
        elif inst in SC.get("detail_missing", []):
            detail = "DETAIL=MISSING"
        else:
            m = re.search(r"s3://\S+/(\S+\.detail\.json)", cmd)
            os.makedirs(BUCKET_DIR, exist_ok=True)
            with open(os.path.join(BUCKET_DIR, os.path.basename(m.group(1))), "w") as f:
                json.dump(SC.get("details", {}).get(inst, {}), f)
            detail = "DETAIL=UPLOADED"
        return "\n".join([per_instance("recv_done", inst, "RECV=DONE"), "=====RESULT=====", res,
                          "=====DETAIL=====", detail, "=====CLOCK=====",
                          "system_time_us=%s\nleap=Normal" % per_instance("post_clock", inst, "0.2")])
    if "preflight.sh --clock" in cmd:
        return "system_time_us=%s\nleap=Normal" % per_instance("post_clock", inst, "0.2")
    if "RECV=READY" in cmd:         # one command: stop an old receiver, start this one, wait until ready
        seq = SC.get("recv_ready", {}).get(inst, ["RECV=READY"])
        n = counter("ready." + inst)
        return seq[min(n, len(seq)) - 1]
    if re.search(r"mcast_send(\.py)? \d", cmd):
        return SC.get("sender", "")
    if "sockperf server --ip" in cmd:
        return "SERVER=READY"
    if "sockperf under-load" in cmd or "cat /tmp/sockperf_result.txt" in cmd:
        return "sockperf: Summary: Latency is 80.0 usec\navg-rtt=80.0\npercentile 50.000 = 79.0\npercentile 99.000 = 120.0\n<MAX> observation = 300.0\n<MIN> observation = 70.0\n"
    return ""


def status_for(inst, cmd):
    if re.search(r"mcast_send(\.py)? \d", cmd) and SC.get("sender_status"):
        return SC["sender_status"]
    return "Success"


if sys.argv[1] == "__ssh__":
    # Stand-in for `ssh [options] user@host bash -s`: the script arrives on stdin.
    args = sys.argv[2:]
    if "-O" in args:
        sys.exit(0)
    positional, i = [], 0
    while i < len(args):
        if args[i] in ("-i", "-o", "-S", "-p", "-l", "-F"):
            i += 2
            continue
        if args[i].startswith("-"):
            i += 1
            continue
        positional.append(args[i])
        i += 1
    host = positional[0].split("@")[-1]
    inst = next((k for k, v in SC.get("public_ips", {}).items() if v == host), None)
    if inst is None:
        sys.stderr.write("fake ssh: unknown host %s\n" % host)
        sys.exit(255)
    failures = SC.get("ssh_fail_first", {}).get(inst, 0)
    if counter("sshconn." + inst) <= failures:
        sys.stderr.write("Permission denied (publickey).\n")
        sys.exit(255)
    cmd = sys.stdin.read()
    with open(os.path.join(STATE, "calls.jsonl"), "a") as f:
        f.write(json.dumps({"id": counter("cmdid"), "instance": inst, "command": cmd, "transport": "ssh"}) + "\n")
    out(respond(inst, cmd))
    sys.exit(1 if status_for(inst, cmd) == "Failed" else 0)

svc, op = sys.argv[1], sys.argv[2]
with open(os.path.join(STATE, "api.log"), "a") as _f:   # every non-command call, for "no EC2 or CloudFormation on a repeat run"
    _f.write(svc + " " + op + "\n")
query = arg("--query", "")

if svc == "cloudformation" and not query:
    sys.stdout.write(json.dumps({"Stacks": [{
        "StackId": "arn:aws:cloudformation:us-east-1:123456789012:stack/S/guid",
        "Outputs": [{"OutputKey": k, "OutputValue": v} for k, v in SC["outputs"].items()]}]}))
elif svc == "cloudformation":
    if "StackId" in query:
        out("arn:aws:cloudformation:us-east-1:123456789012:stack/S/guid")
    else:
        key = re.search(r"OutputKey=='(\w+)'", query).group(1)
        out(SC["outputs"][key])
elif svc == "ec2":
    inst = arg("--instance-ids")
    if "PublicIpAddress" in query and "InstanceId" in query:   # the whole table, one row per instance
        ids = []
        for tok in sys.argv[sys.argv.index("--instance-ids") + 1:]:
            if tok.startswith("--"):
                break
            ids.append(tok)
        rows = []
        for i in ids:
            rows.append("\t".join([i, SC.get("public_ips", {}).get(i, "None")]))
        out("\n".join(rows))
    elif "ImageId" in query:
        out("ami-0123456789abcdef0")
    elif "PrivateIpAddress" in query:
        out(SC["ips"].get(inst, "10.0.0.9"))
    elif "AvailabilityZone" in query:
        out(SC["az"].get(inst, "us-east-1a"))
    else:
        out(SC.get("instance_type", "m7i.large"))
elif svc == "ec2-instance-connect":
    inst = arg("--instance-id")
    with open(os.path.join(STATE, "calls.jsonl"), "a") as f:
        f.write(json.dumps({"id": counter("cmdid"), "instance": inst, "command": "__push_key__"}) + "\n")
    out("True")
elif svc == "ssm" and op == "describe-instance-information":
    if "Online" in query:   # the ids of the online instances among those asked for
        ids = arg("--filters").split("Values=")[1].split(",")
        out("\t".join(i for i in ids if i not in SC.get("ssm_offline", [])))
    else:
        out("Online")
elif svc == "ssm" and op == "send-command":
    inst = arg("--instance-ids")
    if inst in SC.get("gone", []):   # a terminated or replaced instance
        sys.stderr.write("An error occurred (InvalidInstanceId) when calling the SendCommand operation\n")
        sys.exit(254)
    n = counter("cmdid")
    cmd = json.loads(arg("--parameters"))["commands"][0]
    with open(os.path.join(STATE, "calls.jsonl"), "a") as f:
        f.write(json.dumps({"id": n, "instance": inst, "command": cmd}) + "\n")
    with open(os.path.join(STATE, "cmd.%d.json" % n), "w") as f:
        json.dump({"instance": inst, "command": cmd}, f)
    out("cmd-%d" % n)
elif svc == "ssm" and op == "get-command-invocation":
    n = int(arg("--command-id").split("-")[1])
    rec = json.load(open(os.path.join(STATE, "cmd.%d.json" % n)))
    if query == "Status":
        out(status_for(rec["instance"], rec["command"]))
    elif query:
        out(respond(rec["instance"], rec["command"]))
    else:   # no --query: the full JSON document, as the orchestrator's single poll call reads it
        sys.stdout.write(json.dumps({"Status": status_for(rec["instance"], rec["command"]),
                                     "StandardOutputContent": respond(rec["instance"], rec["command"])}))
elif svc == "s3" and op == "cp":
    dest = sys.argv[4]
    os.makedirs(dest, exist_ok=True)
    if os.path.isdir(BUCKET_DIR):
        for name in os.listdir(BUCKET_DIR):
            shutil.copy(os.path.join(BUCKET_DIR, name), dest)
else:
    sys.stderr.write("fake aws: unhandled %s\n" % sys.argv[1:])
    sys.exit(2)

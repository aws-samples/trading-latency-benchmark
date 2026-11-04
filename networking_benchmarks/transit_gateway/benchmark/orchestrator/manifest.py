"""What `prepare` learned about a stack, kept on the operator's machine between runs.

It holds the stack identity (from CloudFormation) and, per instance: the facts read from the
instance's metadata service, the boot id, the sha256 of the installed helper files, the native
build and the static host context. Everything is valid until an instance reboots or is replaced;
a run detects either with its one per-instance gate call.
"""
from __future__ import annotations

import hashlib
import json
import os
import time
from typing import Any, Dict, List, Optional

VERSION = 2
REMOTE_BIN = "/opt/benchmark/bin"
# Files installed on every instance under REMOTE_BIN, and where each lives relative to benchmark/.
# The multicast sender and receiver are built on the instances from host/cpp, not installed from here.
HELPER_SOURCES = {
    "preflight.sh": "host/preflight.sh",
}
HELPERS = tuple(HELPER_SOURCES)

Manifest = Dict[str, Any]


def local_hashes(bench_dir: str) -> Dict[str, str]:
    """sha256 of each helper file as it is on this machine right now."""
    out = {}
    for name in HELPERS:
        with open(os.path.join(bench_dir, HELPER_SOURCES[name]), "rb") as f:
            out[name] = hashlib.sha256(f.read()).hexdigest()
    return out


def path_for(state_dir: str, stack_name: str) -> str:
    return os.path.join(state_dir, f"{stack_name}.json")


def load(state_dir: str, stack_name: str) -> Optional[Manifest]:
    try:
        with open(path_for(state_dir, stack_name)) as f:
            m = json.load(f)
    except (OSError, ValueError):
        return None
    return m if isinstance(m, dict) and m.get("version") == VERSION else None


def save(state_dir: str, stack_name: str, manifest: Manifest) -> None:
    os.makedirs(state_dir, exist_ok=True)
    tmp = path_for(state_dir, stack_name) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    os.replace(tmp, path_for(state_dir, stack_name))


def new(stack: Dict[str, Any], instances: Dict[str, Dict[str, Any]]) -> Manifest:
    return {"version": VERSION, "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "stack": stack, "instances": instances}


def manifest_problems(manifest: Optional[Manifest], hashes: Dict[str, str]) -> List[str]:
    """Reasons the manifest cannot be used before asking any instance (empty list: usable)."""
    if manifest is None:
        return ["no manifest for this stack"]
    if any(rec.get("helpers") != hashes for rec in manifest.get("instances", {}).values()):
        return ["helper files changed on this machine"]
    return []


def instance_problems(rec: Dict[str, Any], boot_id: str, remote_hashes: Dict[str, str]) -> List[str]:
    """Reasons one instance no longer matches what was prepared."""
    if not boot_id:
        return ["unreachable (replaced or stopped?)"]
    problems = []
    if rec.get("boot_id") != boot_id:
        problems.append("rebooted")
    if remote_hashes != rec.get("helpers"):
        problems.append("helper files differ")
    return problems

#!/usr/bin/env python3
"""
report.py - Parse the NxN latency matrix and produce the batch heatmap report.

Also the shared data layer: the loaders here (load_fleet_metadata, build_matrix,
_build_topology_fleet_json) are imported by fleet_json.py to emit fleet.json.

Reads all <src_ip>-<dst_ip>.json files in the results directory, builds an NxN
latency matrix, and generates:
  - A terminal-formatted matrix table (p50, p99)
  - An HTML heatmap report (matrix_report.html)
  - Asymmetry analysis (A→B vs B→A differences)
  - Per-instance-type grouping statistics
  - Consolidated JSON matrix (matrix_summary.json)

The interactive 2D/3D topology view now lives in report/web/ (Vite + three.js),
which renders fleet.json (produced by fleet_json.py).

Usage: python3 report.py <results_dir>
"""

import json
import math
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ─── Instance type metadata lookup table ──────────────────────────────────────
# Used when fleet.json doesn't include per-node hardware metadata.
INSTANCE_METADATA: Dict[str, Dict[str, Any]] = {
    # c7i family (Intel Sapphire Rapids, Nitro 5)
    "c7i.xlarge":    {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 4,   "mem_gb": 8,    "metal": False},
    "c7i.2xlarge":   {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 8,   "mem_gb": 16,   "metal": False},
    "c7i.4xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 16,  "mem_gb": 32,   "metal": False},
    "c7i.8xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 32,  "mem_gb": 64,   "metal": False},
    "c7i.12xlarge":  {"enis": 8,  "bw_gbps": 37.5, "pps_mpps": 7.5, "nitro_gen": 5, "vcpus": 48,  "mem_gb": 96,   "metal": False},
    "c7i.16xlarge":  {"enis": 15, "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 5, "vcpus": 64,  "mem_gb": 128,  "metal": False},
    "c7i.24xlarge":  {"enis": 15, "bw_gbps": 75,   "pps_mpps": 15,  "nitro_gen": 5, "vcpus": 96,  "mem_gb": 192,  "metal": False},
    "c7i.metal-24xl": {"enis": 15, "bw_gbps": 75,  "pps_mpps": 15,  "nitro_gen": 5, "vcpus": 96,  "mem_gb": 192,  "metal": True},
    "c7i.48xlarge":  {"enis": 15, "bw_gbps": 100,  "pps_mpps": 20,  "nitro_gen": 5, "vcpus": 192, "mem_gb": 384,  "metal": False},
    "c7i.metal-48xl": {"enis": 15, "bw_gbps": 100, "pps_mpps": 20,  "nitro_gen": 5, "vcpus": 192, "mem_gb": 384,  "metal": True},
    # c6in family (Intel Ice Lake, Nitro 4, enhanced networking)
    "c6in.xlarge":   {"enis": 4,  "bw_gbps": 30,   "pps_mpps": 6,   "nitro_gen": 4, "vcpus": 4,   "mem_gb": 8,    "metal": False},
    "c6in.2xlarge":  {"enis": 4,  "bw_gbps": 40,   "pps_mpps": 8,   "nitro_gen": 4, "vcpus": 8,   "mem_gb": 16,   "metal": False},
    "c6in.4xlarge":  {"enis": 8,  "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 4, "vcpus": 16,  "mem_gb": 32,   "metal": False},
    "c6in.8xlarge":  {"enis": 8,  "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 4, "vcpus": 32,  "mem_gb": 64,   "metal": False},
    "c6in.12xlarge": {"enis": 8,  "bw_gbps": 75,   "pps_mpps": 15,  "nitro_gen": 4, "vcpus": 48,  "mem_gb": 96,   "metal": False},
    "c6in.16xlarge": {"enis": 15, "bw_gbps": 100,  "pps_mpps": 20,  "nitro_gen": 4, "vcpus": 64,  "mem_gb": 128,  "metal": False},
    "c6in.24xlarge": {"enis": 15, "bw_gbps": 150,  "pps_mpps": 30,  "nitro_gen": 4, "vcpus": 96,  "mem_gb": 192,  "metal": False},
    "c6in.32xlarge": {"enis": 15, "bw_gbps": 200,  "pps_mpps": 40,  "nitro_gen": 4, "vcpus": 128, "mem_gb": 256,  "metal": False},
    "c6in.metal":    {"enis": 15, "bw_gbps": 200,  "pps_mpps": 40,  "nitro_gen": 4, "vcpus": 128, "mem_gb": 256,  "metal": True},
    # m7i family (Intel Sapphire Rapids, Nitro 5)
    "m7i.xlarge":    {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 4,   "mem_gb": 16,   "metal": False},
    "m7i.2xlarge":   {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 8,   "mem_gb": 32,   "metal": False},
    "m7i.4xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 16,  "mem_gb": 64,   "metal": False},
    "m7i.8xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 32,  "mem_gb": 128,  "metal": False},
    "m7i.12xlarge":  {"enis": 8,  "bw_gbps": 37.5, "pps_mpps": 7.5, "nitro_gen": 5, "vcpus": 48,  "mem_gb": 192,  "metal": False},
    "m7i.16xlarge":  {"enis": 15, "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 5, "vcpus": 64,  "mem_gb": 256,  "metal": False},
    "m7i.24xlarge":  {"enis": 15, "bw_gbps": 75,   "pps_mpps": 15,  "nitro_gen": 5, "vcpus": 96,  "mem_gb": 384,  "metal": False},
    "m7i.48xlarge":  {"enis": 15, "bw_gbps": 100,  "pps_mpps": 20,  "nitro_gen": 5, "vcpus": 192, "mem_gb": 768,  "metal": False},
    # r7i family (Intel Sapphire Rapids, Nitro 5, memory-optimized)
    "r7i.xlarge":    {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 4,   "mem_gb": 32,   "metal": False},
    "r7i.2xlarge":   {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 5, "vcpus": 8,   "mem_gb": 64,   "metal": False},
    "r7i.4xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 16,  "mem_gb": 128,  "metal": False},
    "r7i.8xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 5, "vcpus": 32,  "mem_gb": 256,  "metal": False},
    "r7i.12xlarge":  {"enis": 8,  "bw_gbps": 37.5, "pps_mpps": 7.5, "nitro_gen": 5, "vcpus": 48,  "mem_gb": 384,  "metal": False},
    "r7i.16xlarge":  {"enis": 15, "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 5, "vcpus": 64,  "mem_gb": 512,  "metal": False},
    "r7i.24xlarge":  {"enis": 15, "bw_gbps": 75,   "pps_mpps": 15,  "nitro_gen": 5, "vcpus": 96,  "mem_gb": 768,  "metal": False},
    # c6i family (Intel Ice Lake, Nitro 4)
    "c6i.xlarge":    {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 4, "vcpus": 4,   "mem_gb": 8,    "metal": False},
    "c6i.2xlarge":   {"enis": 4,  "bw_gbps": 12.5, "pps_mpps": 2,   "nitro_gen": 4, "vcpus": 8,   "mem_gb": 16,   "metal": False},
    "c6i.4xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 4, "vcpus": 16,  "mem_gb": 32,   "metal": False},
    "c6i.8xlarge":   {"enis": 8,  "bw_gbps": 25,   "pps_mpps": 5,   "nitro_gen": 4, "vcpus": 32,  "mem_gb": 64,   "metal": False},
    "c6i.metal":     {"enis": 15, "bw_gbps": 50,   "pps_mpps": 10,  "nitro_gen": 4, "vcpus": 128, "mem_gb": 256,  "metal": True},
}

# Fallback metadata for unknown instance types (derive from size suffix)
_SIZE_VCPUS = {
    "xlarge": 4, "2xlarge": 8, "4xlarge": 16, "8xlarge": 32,
    "12xlarge": 48, "16xlarge": 64, "24xlarge": 96, "48xlarge": 192,
}


def _default_metadata_for_type(instance_type: str) -> Dict[str, Any]:
    """Generate sensible defaults for an unknown instance type."""
    parts = instance_type.split(".")
    family = parts[0] if parts else "unknown"
    size = parts[1] if len(parts) > 1 else "xlarge"
    vcpus = _SIZE_VCPUS.get(size, 4)
    is_metal = "metal" in size
    return {
        "enis": 8 if vcpus >= 16 else 4,
        "bw_gbps": min(vcpus * 1.5, 100),
        "pps_mpps": min(vcpus * 0.3, 20),
        "nitro_gen": 5 if "7" in family else 4,
        "vcpus": vcpus,
        "mem_gb": vcpus * 4 if "c" in family else vcpus * 8,
        "metal": is_metal,
    }


def get_instance_metadata(instance_type: str) -> Dict[str, Any]:
    """Look up or infer hardware metadata for an instance type."""
    if instance_type in INSTANCE_METADATA:
        return INSTANCE_METADATA[instance_type]
    return _default_metadata_for_type(instance_type)


# ─── Core data loading and parsing ────────────────────────────────────────────


def load_fleet_metadata(results_dir: Path) -> dict:
    """Load fleet metadata for the run.

    Prefers a consolidated ``fleet.json`` (``{"nodes": [...]}``) when present.
    Otherwise assembles the fleet from the per-node ``<ip>_metadata.json`` files
    that ``run_ucast.yaml`` writes (fields: ``instance_type``, ``az``, ``region``,
    ``pg_name``, ``pg_type``, ``private_ip``, ``hostname`` and — for newer runs —
    ``account``/``vpc_id``).

    An account id may be injected via the ``AFXDP_ACCOUNT`` env var to stamp runs
    whose metadata predates account capture (used only to draw the account
    boundary; it does not alter measured data).
    """
    account_override = os.environ.get("AFXDP_ACCOUNT")

    fleet_path = results_dir / "fleet.json"
    if fleet_path.exists():
        try:
            fleet = json.loads(fleet_path.read_text())
            if account_override:
                fleet.setdefault("account", account_override)
                for nd in fleet.get("nodes", []):
                    nd.setdefault("account", account_override)
            # Backfill role from per-node metadata for older fleet.json that
            # predate role capture, so the viewer can distinguish the relay.
            for nd in fleet.get("nodes", []):
                if nd.get("role"):
                    continue
                mf = results_dir / f"{nd.get('private_ip') or nd.get('name')}_metadata.json"
                if mf.exists():
                    try:
                        nd["role"] = json.loads(mf.read_text()).get("role") or "unknown"
                    except json.JSONDecodeError:
                        nd["role"] = "unknown"
                else:
                    nd["role"] = "unknown"
            return fleet
        except json.JSONDecodeError:
            print(f"  Warning: could not parse {fleet_path}")

    # Assemble from per-node <ip>_metadata.json files.
    nodes: List[Dict[str, Any]] = []
    for mf in sorted(results_dir.glob("*_metadata.json")):
        try:
            m = json.loads(mf.read_text())
        except json.JSONDecodeError:
            print(f"  Warning: could not parse {mf}")
            continue
        ip = m.get("private_ip") or mf.stem.replace("_metadata", "")
        nodes.append({
            "name": ip,
            "private_ip": ip,
            "public_ip": m.get("hostname") or m.get("public_ip") or "",
            "ec2_name": m.get("ec2_name") or ip,
            "type": m.get("instance_type") or "unknown",
            "az": m.get("az") or "unknown",
            "region": m.get("region") or "unknown",
            "account": m.get("account") or account_override or "unknown",
            "vpc_id": m.get("vpc_id") or "unknown",
            "cpg_name": m.get("pg_name") or "unknown",
            "pg_type": m.get("pg_type") or "unknown",
            "role": m.get("role") or "unknown",
        })

    if not nodes:
        return {"account": account_override} if account_override else {}

    return {
        "nodes": nodes,
        "region": nodes[0]["region"],
        "account": nodes[0]["account"],
    }


def parse_result_json(filepath: Path) -> Optional[dict]:
    """Parse a rtt JSON result file."""
    try:
        raw = json.loads(filepath.read_text())
        svc = raw.get("service_rtt_us", {})
        if not svc:
            return None
        # Multicast per-pair JSON also carries the two-hop split (source->replicator,
        # replicator->dest); ucast results omit these (left as None). Surfacing them
        # is the whole point of the mcast measurement, so thread them through.
        hop1 = raw.get("hop1_us") or {}
        hop2 = raw.get("hop2_us") or {}
        return {
            "min_us": svc.get("min", 0),
            "p50_us": svc.get("p50", 0),
            "p90_us": svc.get("p90", 0),
            "p95_us": svc.get("p95", 0),
            "p99_us": svc.get("p99", 0),
            "p999_us": svc.get("p999", 0),
            "max_us": svc.get("max", 0),
            "mean_us": svc.get("mean", 0),
            "hop1_p50_us": hop1.get("p50"),
            "hop1_p99_us": hop1.get("p99"),
            "hop2_p50_us": hop2.get("p50"),
            "hop2_p99_us": hop2.get("p99"),
            "messages": raw.get("messages", 0),
            "lost": raw.get("lost", 0),
            "loss_pct": raw.get("loss_pct", 0.0),
            "timestamp_rx": raw.get("timestamp_rx", "unknown"),
            "timestamp_tx": raw.get("timestamp_tx", "unknown"),
        }
    except (json.JSONDecodeError, FileNotFoundError, KeyError):
        return None


def parse_result_txt(filepath: Path) -> Optional[dict]:
    """Fallback: parse rtt text output if JSON is missing."""
    try:
        text = filepath.read_text()
    except FileNotFoundError:
        return None
    patterns = {
        "p50_us": r"p50\s*[=:]\s*(\d+)",
        "p90_us": r"p90\s*[=:]\s*(\d+)",
        "p95_us": r"p95\s*[=:]\s*(\d+)",
        "p99_us": r"p99\s*[=:]\s*(\d+)",
        "p999_us": r"p99\.9\s*[=:]\s*(\d+)",
        "min_us": r"min\s*[=:]\s*(\d+)",
        "max_us": r"max\s*[=:]\s*(\d+)",
        "mean_us": r"mean\s*[=:]\s*([\d.]+)",
    }
    data: Dict[str, Any] = {}
    for key, pattern in patterns.items():
        m = re.search(pattern, text, re.IGNORECASE)
        if m:
            val = m.group(1)
            data[key] = float(val) if "." in val else int(val)
    return data if data.get("p50_us") else None


def build_matrix(results_dir: Path, fleet: dict) -> Tuple[List[str], Dict[Tuple[str, str], dict]]:
    """Build the NxN matrix from result files.

    Supports both naming conventions:
      - New: <src_ip>-<dst_ip>.json (e.g. 10.61.0.197-10.61.0.205.json)
      - Legacy: <src>_to_<dst>.json (e.g. node0_to_node1.json)

    Returns (node_names, matrix_dict) where matrix_dict[(src,dst)] = latency_data.
    """
    nodes = fleet.get("nodes", [])
    node_names = [n["name"] for n in nodes] if nodes else []
    matrix: Dict[Tuple[str, str], dict] = {}

    # Try new naming first: <src_ip>-<dst_ip>.json (IP addresses contain dots)
    ip_pattern = re.compile(r"^(\d+\.\d+\.\d+\.\d+)-(\d+\.\d+\.\d+\.\d+)$")
    found_new = False
    for jf in sorted(results_dir.glob("*.json")):
        if jf.stem.endswith("_metadata") or jf.stem in ("matrix_summary", "fleet"):
            continue
        m = ip_pattern.match(jf.stem)
        if m:
            found_new = True
            src_name, dst_name = m.group(1), m.group(2)
            data = parse_result_json(jf)
            if data:
                matrix[(src_name, dst_name)] = data
                if src_name not in node_names:
                    node_names.append(src_name)
                if dst_name not in node_names:
                    node_names.append(dst_name)

    # Fallback: legacy *_to_*.json naming
    if not found_new:
        for jf in sorted(results_dir.glob("*_to_*.json")):
            stem = jf.stem
            m = re.match(r"(.+?)_to_(.+)", stem)
            if not m:
                continue
            src_name, dst_name = m.group(1), m.group(2)

            data = parse_result_json(jf)
            if not data:
                txt = jf.with_suffix(".txt")
                if txt.exists():
                    data = parse_result_txt(txt)
            if data:
                matrix[(src_name, dst_name)] = data
                if src_name not in node_names:
                    node_names.append(src_name)
                if dst_name not in node_names:
                    node_names.append(dst_name)

    # ── Multicast: render the real datapath as two hops through the relay ──
    # A measured pair is source->dest end-to-end, but the packet actually travels
    # source -> replicator -> dest. Replace each such direct edge with two hop
    # edges — hop1 (src->relay) and hop2 (relay->dst) — so the topology shows the
    # true path and the replicator becomes a rendered waypoint instead of an
    # isolated node. Only when exactly one replicator exists (routing is then
    # unambiguous); otherwise the direct edges are left as-is.
    replicators = [n["name"] for n in nodes if n.get("role") == "replicator"]
    if len(replicators) == 1:
        relay = replicators[0]
        hop_edges: Dict[Tuple[str, str], dict] = {}
        drop: List[Tuple[str, str]] = []
        for (s, d), data in matrix.items():
            if s == relay or d == relay:
                continue
            h1, h2 = data.get("hop1_p50_us"), data.get("hop2_p50_us")
            if h1 is None and h2 is None:
                continue
            msgs, loss = data.get("messages", 0), data.get("loss_pct", 0.0)
            if h1 is not None:
                hop_edges[(s, relay)] = {"p50_us": h1, "p99_us": data.get("hop1_p99_us", 0),
                                         "loss_pct": loss, "messages": msgs, "hop_kind": "hop1"}
            if h2 is not None:
                hop_edges[(relay, d)] = {"p50_us": h2, "p99_us": data.get("hop2_p99_us", 0),
                                         "loss_pct": loss, "messages": msgs, "hop_kind": "hop2"}
            drop.append((s, d))
        for k in drop:
            matrix.pop(k, None)
        matrix.update(hop_edges)

    node_names = sorted(set(node_names))
    return node_names, matrix


# ─── Terminal output ──────────────────────────────────────────────────────────


def generate_terminal_table(node_names: List[str], matrix: dict, metric: str = "p50_us") -> None:
    """Print NxN matrix table to stdout."""
    n = len(node_names)
    short_names = []
    for name in node_names:
        parts = name.split("-")
        idx = parts[1] if len(parts) > 1 else "?"
        itype = "-".join(parts[2:3]) if len(parts) > 2 else "?"
        short_names.append(f"n{idx}:{itype}")

    col_width = max(10, max(len(s) for s in short_names) + 2)
    header_metric = metric.replace("_us", "").upper()

    print(f"\n{'=' * (col_width * (n + 1) + 4)}")
    print(f"  LATENCY MATRIX ({header_metric}, microseconds) — src→dst")
    print(f"{'─' * (col_width * (n + 1) + 4)}")

    print(f"{'FROM \\ TO':<{col_width}}", end="")
    for name in short_names:
        print(f"{name:>{col_width}}", end="")
    print()
    print("─" * (col_width * (n + 1) + 4))

    for i, src in enumerate(node_names):
        print(f"{short_names[i]:<{col_width}}", end="")
        for j, dst in enumerate(node_names):
            if i == j:
                print(f"{'—':>{col_width}}", end="")
            else:
                data = matrix.get((src, dst))
                if data:
                    val = data.get(metric, 0)
                    print(f"{val:>{col_width}}", end="")
                else:
                    print(f"{'N/A':>{col_width}}", end="")
        print()

    print(f"{'=' * (col_width * (n + 1) + 4)}")


# ─── Analysis helpers ─────────────────────────────────────────────────────────


def compute_asymmetry(node_names: List[str], matrix: dict) -> List[dict]:
    """Compute A→B vs B→A asymmetry for all pairs."""
    asymmetries = []
    seen: set = set()
    for i, a in enumerate(node_names):
        for j, b in enumerate(node_names):
            if i >= j:
                continue
            pair_key = (min(a, b), max(a, b))
            if pair_key in seen:
                continue
            seen.add(pair_key)

            ab = matrix.get((a, b), {}).get("p50_us")
            ba = matrix.get((b, a), {}).get("p50_us")
            if ab and ba:
                diff = abs(ab - ba)
                pct = (diff / min(ab, ba)) * 100 if min(ab, ba) > 0 else 0
                asymmetries.append({
                    "pair": f"{a} ↔ {b}",
                    "a_to_b_p50": ab,
                    "b_to_a_p50": ba,
                    "diff_us": diff,
                    "diff_pct": round(pct, 1),
                })
    return sorted(asymmetries, key=lambda x: x["diff_us"], reverse=True)


def compute_type_stats(fleet: dict, matrix: dict) -> dict:
    """Group results by instance type pair and compute aggregate stats."""
    nodes = {n["name"]: n["type"] for n in fleet.get("nodes", [])}
    type_pairs: Dict[str, List[int]] = {}

    for (src, dst), data in matrix.items():
        src_type = nodes.get(src, "unknown")
        dst_type = nodes.get(dst, "unknown")
        pair_label = f"{src_type} → {dst_type}"
        if pair_label not in type_pairs:
            type_pairs[pair_label] = []
        p50 = data.get("p50_us", 0)
        if p50 > 0:
            type_pairs[pair_label].append(p50)

    stats = {}
    for label, values in sorted(type_pairs.items()):
        if values:
            stats[label] = {
                "count": len(values),
                "min_p50": min(values),
                "max_p50": max(values),
                "mean_p50": round(sum(values) / len(values), 1),
                "spread": max(values) - min(values),
            }
    return stats


# ─── HTML heatmap report (original) ──────────────────────────────────────────


def color_for_value(value: float, vmin: float, vmax: float) -> str:
    """Generate a CSS color from green (low latency) to red (high latency)."""
    if vmax == vmin:
        return "#4CAF50"
    t = min(1.0, max(0.0, (value - vmin) / (vmax - vmin)))
    if t < 0.5:
        r = int(76 + (255 - 76) * (t * 2))
        g = int(175 + (235 - 175) * (t * 2))
        b = int(80 - 80 * (t * 2))
    else:
        t2 = (t - 0.5) * 2
        r = 255
        g = int(235 - 235 * t2)
        b = 0
    return f"#{r:02x}{g:02x}{b:02x}"


def fmt_lat(us) -> str:
    """Format microseconds: >=500us -> ms (0.5 ms), >=500ms -> s (0.5 s)."""
    try:
        v = float(us)
    except (TypeError, ValueError):
        return "\u2014"
    if v <= 0:
        return "\u2014"
    def trim(x: float) -> str:
        return f"{x:.2f}".rstrip("0").rstrip(".")
    if v >= 500000:
        return f"{trim(v / 1e6)} s"
    if v >= 500:
        return f"{trim(v / 1e3)} ms"
    return f"{int(round(v))} \u03bcs"


def generate_html_report(node_names: List[str], matrix: dict, fleet: dict,
                         asymmetries: list, type_stats: dict, output_path: Path) -> None:
    """Generate a full HTML heatmap report (matrix_report.html)."""
    n = len(node_names)
    nodes_meta = {nd["name"]: nd for nd in fleet.get("nodes", [])}

    all_p50 = [d.get("p50_us", 0) for d in matrix.values() if d.get("p50_us", 0) > 0]
    all_p99 = [d.get("p99_us", 0) for d in matrix.values() if d.get("p99_us", 0) > 0]
    vmin_p50 = min(all_p50) if all_p50 else 0
    vmax_p50 = max(all_p50) if all_p50 else 100
    vmin_p99 = min(all_p99) if all_p99 else 0
    vmax_p99 = max(all_p99) if all_p99 else 100

    def short_name(name: str) -> str:
        meta = nodes_meta.get(name, {})
        idx = meta.get("index", "?")
        itype = meta.get("type", name.split("-", 2)[-1] if "-" in name else name)
        return f"n{idx}<br><small>{itype}</small>"

    def build_heatmap(metric: str, vmin: float, vmax: float) -> str:
        rows = ""
        for i, src in enumerate(node_names):
            cells = f"<td class='row-hdr'>{short_name(src)}</td>"
            for j, dst in enumerate(node_names):
                if i == j:
                    cells += "<td class='diag'>—</td>"
                else:
                    data = matrix.get((src, dst))
                    if data and data.get(metric, 0) > 0:
                        val = data[metric]
                        color = color_for_value(val, vmin, vmax)
                        tooltip = (f"{src} → {dst}\\n"
                                   f"p50={fmt_lat(data.get('p50_us',0))}  "
                                   f"p99={fmt_lat(data.get('p99_us',0))}  "
                                   f"p99.9={fmt_lat(data.get('p999_us',0))}  "
                                   f"max={fmt_lat(data.get('max_us',0))}  "
                                   f"loss={data.get('loss_pct',0):.2f}%")
                        # Multicast: mark which hop of the src->relay->dst path this is.
                        if data.get("hop_kind"):
                            tooltip += f"\\n[{data['hop_kind']} of the multicast fan-out path]"
                        cells += (f"<td style='background:{color}' title='{tooltip}'>"
                                  f"<strong>{fmt_lat(val)}</strong></td>")
                    else:
                        cells += "<td class='na'>N/A</td>"
            rows += f"<tr>{cells}</tr>\n"
        header_cells = "<th></th>" + "".join(f"<th>{short_name(n)}</th>" for n in node_names)
        return f"<table class='matrix'><tr>{header_cells}</tr>\n{rows}</table>"

    asym_rows = ""
    for a in asymmetries[:10]:
        asym_rows += (f"<tr><td>{a['pair']}</td><td>{fmt_lat(a['a_to_b_p50'])}</td>"
                      f"<td>{fmt_lat(a['b_to_a_p50'])}</td><td>{fmt_lat(a['diff_us'])}</td>"
                      f"<td>{a['diff_pct']}%</td></tr>\n")

    type_rows = ""
    for label, s in type_stats.items():
        type_rows += (f"<tr><td>{label}</td><td>{s['count']}</td><td>{fmt_lat(s['min_p50'])}</td>"
                      f"<td>{fmt_lat(s['max_p50'])}</td><td>{fmt_lat(s['mean_p50'])}</td><td>{fmt_lat(s['spread'])}</td></tr>\n")

    pairs_measured = len(matrix)
    total_pairs = n * (n - 1)
    median_p50 = sorted(all_p50)[len(all_p50) // 2] if all_p50 else 0
    median_p99 = sorted(all_p99)[len(all_p99) // 2] if all_p99 else 0

    html = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>AF_XDP Latency Matrix Report</title>
<style>
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; margin: 2em; background: #fafafa; color: #333; }}
h1 {{ color: #1a1a1a; border-bottom: 2px solid #ff9900; padding-bottom: 0.5em; }}
h2 {{ color: #232f3e; margin-top: 2em; }}
.summary {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 1em; margin: 1em 0; }}
.stat-card {{ background: white; border: 1px solid #e0e0e0; border-radius: 8px; padding: 1em; text-align: center; }}
.stat-card .value {{ font-size: 2em; font-weight: bold; color: #232f3e; }}
.stat-card .label {{ font-size: 0.85em; color: #666; margin-top: 0.3em; }}
table.matrix {{ border-collapse: collapse; margin: 1em 0; }}
table.matrix th, table.matrix td {{ border: 1px solid #ccc; padding: 8px 12px; text-align: center; min-width: 60px; }}
table.matrix th {{ background: #232f3e; color: white; font-size: 0.8em; }}
table.matrix td.diag {{ background: #f0f0f0; color: #999; }}
table.matrix td.na {{ background: #f5f5f5; color: #aaa; }}
table.matrix td.row-hdr {{ background: #232f3e; color: white; font-size: 0.8em; text-align: left; }}
table.matrix td strong {{ font-size: 0.9em; }}
table.data {{ border-collapse: collapse; margin: 1em 0; width: auto; }}
table.data th, table.data td {{ border: 1px solid #ddd; padding: 6px 12px; text-align: right; }}
table.data th {{ background: #232f3e; color: white; }}
table.data td:first-child {{ text-align: left; }}
.legend {{ display: flex; align-items: center; gap: 0.5em; margin: 0.5em 0; }}
.legend-bar {{ width: 200px; height: 20px; border-radius: 4px;
  background: linear-gradient(to right, #4CAF50, #FFEB3B, #FF0000); }}
.meta {{ font-size: 0.85em; color: #666; margin-top: 2em; padding-top: 1em; border-top: 1px solid #ddd; }}
</style></head><body>
<h1>🔥 AF_XDP Latency Matrix</h1>
<div class="summary">
  <div class="stat-card"><div class="value">{n}</div><div class="label">Nodes</div></div>
  <div class="stat-card"><div class="value">{pairs_measured}/{total_pairs}</div><div class="label">Pairs Measured</div></div>
  <div class="stat-card"><div class="value">{fmt_lat(median_p50)}</div><div class="label">Median p50 RTT</div></div>
  <div class="stat-card"><div class="value">{fmt_lat(median_p99)}</div><div class="label">Median p99 RTT</div></div>
  <div class="stat-card"><div class="value">{fmt_lat(vmax_p50 - vmin_p50)}</div><div class="label">p50 Spread (max−min)</div></div>
</div>
<h2>p50 Heatmap</h2>
<div class="legend"><span>Low</span><div class="legend-bar"></div><span>High</span></div>
{build_heatmap("p50_us", vmin_p50, vmax_p50)}
<h2>p99 Heatmap</h2>
<div class="legend"><span>Low</span><div class="legend-bar"></div><span>High</span></div>
{build_heatmap("p99_us", vmin_p99, vmax_p99)}
<h2>Asymmetry Analysis (top 10)</h2>
<p>Significant A→B vs B→A differences suggest physical path asymmetry.</p>
<table class="data">
<tr><th>Pair</th><th>A→B p50 (μs)</th><th>B→A p50 (μs)</th><th>Δ (μs)</th><th>Δ (%)</th></tr>
{asym_rows}
</table>
<h2>Per Instance-Type Pair Statistics</h2>
<table class="data">
<tr><th>Type Pair</th><th>Samples</th><th>Min p50</th><th>Max p50</th><th>Mean p50</th><th>Spread</th></tr>
{type_rows}
</table>
<div class="meta">
  <p>Generated by <code>report.py</code> | Region: {fleet.get('region', '?')} |
     {fleet.get('messages', '?')} msgs @ {fleet.get('rate', '?')} msg/s |
     Timestamp: {fleet.get('timestamp', '?')}</p>
</div>
</body></html>"""

    output_path.write_text(html)
    print(f"  HTML heatmap: {output_path}")


# ─── Shared topology data model (fleet.json, schema afxdp.topology/v1) ─────────


def _build_topology_fleet_json(node_names: List[str], matrix: dict, fleet: dict) -> str:
    """Build the fleet data object (JSON string) for fleet.json / the web renderer.

    Reads per-node metadata from fleet.json (az, region, vpc_id, cpg_name,
    enis, bw_gbps, etc). Falls back to global fleet values or instance-type
    lookup table for missing fields.
    """
    nodes_meta = {nd["name"]: nd for nd in fleet.get("nodes", [])}
    n = len(node_names)

    # Build per-node JS objects
    js_nodes = []
    for idx, name in enumerate(node_names):
        meta = nodes_meta.get(name, {})
        itype = meta.get("type", "unknown")
        hw = get_instance_metadata(itype)

        node_obj = {
            "index": idx,
            "name": name,
            "ec2_name": meta.get("ec2_name", meta.get("name", name)),
            "type": itype,
            "private_ip": meta.get("private_ip", meta.get("ip", f"10.0.0.{idx + 10}")),
            "public_ip": meta.get("public_ip", ""),
            # Per-node topology fields (fall back to global fleet values)
            "az": meta.get("az", fleet.get("az", "unknown")),
            "region": meta.get("region", fleet.get("region", "unknown")),
            "account": meta.get("account", fleet.get("account", "unknown")),
            "vpc_id": meta.get("vpc_id", fleet.get("vpc_id", "unknown")),
            "cpg_name": meta.get("cpg_name", fleet.get("cpg_name", "unknown")),
            "pg_type": meta.get("pg_type", "unknown"),
            "role": meta.get("role", "unknown"),   # source | replicator | destination
            # Hardware metadata (per-node overrides > lookup table)
            "enis": meta.get("enis", hw["enis"]),
            "bw_gbps": meta.get("bw_gbps", hw["bw_gbps"]),
            "pps_mpps": meta.get("pps_mpps", hw["pps_mpps"]),
            "nitro_gen": meta.get("nitro_gen", hw["nitro_gen"]),
            "vcpus": meta.get("vcpus", hw["vcpus"]),
            "mem_gb": meta.get("mem_gb", hw["mem_gb"]),
            "metal": meta.get("metal", hw["metal"]),
        }
        js_nodes.append(node_obj)

    # Build NxN matrix (indexed by position)
    js_matrix: List[List[Optional[Dict[str, Any]]]] = []
    for i, src in enumerate(node_names):
        row: List[Optional[Dict[str, Any]]] = []
        for j, dst in enumerate(node_names):
            if i == j:
                row.append(None)
            else:
                data = matrix.get((src, dst))
                if data:
                    cell = {
                        "p50": data.get("p50_us", 0),
                        "p90": data.get("p90_us", 0),
                        "p99": data.get("p99_us", 0),
                        "p999": data.get("p999_us", 0),
                        "max": data.get("max_us", 0),
                        "loss": data.get("loss_pct", 0),
                    }
                    # Multicast hop edges (src->relay / relay->dst) are tagged so
                    # the viewer can label the link as hop1/hop2.
                    if data.get("hop_kind"):
                        cell["hop_kind"] = data["hop_kind"]
                    row.append(cell)
                else:
                    row.append(None)
        js_matrix.append(row)

    fleet_obj = {
        "region": fleet.get("region", "unknown"),
        "nodes": js_nodes,
        "matrix": js_matrix,
    }

    return json.dumps(fleet_obj, indent=2)


def main() -> None:
    """Entry point: parse results, generate all outputs."""
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <results_dir>")
        sys.exit(1)

    results_dir = Path(sys.argv[1])
    if not results_dir.is_dir():
        print(f"Error: {results_dir} is not a directory")
        sys.exit(1)

    fleet = load_fleet_metadata(results_dir)
    node_names, matrix = build_matrix(results_dir, fleet)

    if not matrix:
        print("No valid matrix results found.")
        sys.exit(1)

    print(f"\nMatrix: {len(node_names)} nodes, {len(matrix)} directed pairs measured")

    # Terminal output
    generate_terminal_table(node_names, matrix, "p50_us")
    print()
    generate_terminal_table(node_names, matrix, "p99_us")

    # Asymmetry
    asymmetries = compute_asymmetry(node_names, matrix)
    if asymmetries:
        print(f"\nTop asymmetries (p50):")
        for a in asymmetries[:5]:
            print(f"  {a['pair']}: {a['a_to_b_p50']}↔{a['b_to_a_p50']} (Δ{a['diff_us']}μs, {a['diff_pct']}%)")

    # Type stats
    type_stats = compute_type_stats(fleet, matrix)
    if type_stats:
        print(f"\nPer-type pair stats (p50):")
        for label, s in type_stats.items():
            print(f"  {label}: mean={s['mean_p50']}μs, spread={s['spread']}μs ({s['count']} samples)")

    # HTML heatmap report (original)
    html_path = results_dir / "matrix_report.html"
    generate_html_report(node_names, matrix, fleet, asymmetries, type_stats, html_path)

    # JSON export
    json_export = {
        "fleet": fleet,
        "node_names": node_names,
        "matrix": {f"{src}-{dst}": data for (src, dst), data in matrix.items()},
        "asymmetries": asymmetries,
        "type_stats": type_stats,
        "summary": {
            "nodes": len(node_names),
            "pairs_measured": len(matrix),
            "total_pairs": len(node_names) * (len(node_names) - 1),
            "p50_range": [min(d["p50_us"] for d in matrix.values() if d.get("p50_us")),
                          max(d["p50_us"] for d in matrix.values() if d.get("p50_us"))] if matrix else [0, 0],
            "p99_range": [min(d["p99_us"] for d in matrix.values() if d.get("p99_us")),
                          max(d["p99_us"] for d in matrix.values() if d.get("p99_us"))] if matrix else [0, 0],
        },
    }
    json_path = results_dir / "matrix_summary.json"
    json_path.write_text(json.dumps(json_export, indent=2))
    print(f"  JSON summary: {json_path}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""fleet_json.py - emit the canonical topology data model (fleet.json).

The shared data contract (see dev/roadmap.md, schema "afxdp.topology/v1")
consumed by report/web (batch render) and, later, the live collector's WS
snapshot. Reuses the report.py loaders so there is one source of truth.

Usage: python3 fleet_json.py <results_dir> [out.json]
"""

import datetime
import json
import sys
from pathlib import Path

from report import load_fleet_metadata, build_matrix, _build_topology_fleet_json


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <results_dir> [out.json]")
        sys.exit(1)
    results_dir = Path(sys.argv[1])
    if not results_dir.is_dir():
        print(f"Error: {results_dir} is not a directory")
        sys.exit(1)

    fleet = load_fleet_metadata(results_dir)
    node_names, matrix = build_matrix(results_dir, fleet)
    if not matrix:
        print("No matrix results found.")
        sys.exit(1)

    base = json.loads(_build_topology_fleet_json(node_names, matrix, fleet))
    obj = {
        "schema": "afxdp.topology/v1",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "region": base.get("region", "unknown"),
        "account": fleet.get("account", "unknown"),
        "nodes": base.get("nodes", []),
        "matrix": base.get("matrix", []),
    }
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else results_dir / "fleet.json"
    out.write_text(json.dumps(obj, indent=2))
    print(f"  fleet.json: {out}  ({len(obj['nodes'])} nodes)")


if __name__ == "__main__":
    main()

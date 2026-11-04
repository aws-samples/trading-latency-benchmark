"""The blocks of the raw stream. Their format is the contract with results.py."""
from __future__ import annotations

import json
from typing import Dict

from .util import Console


def kv_lines(text: str) -> list:
    return [line for line in text.splitlines() if "=" in line]


def clock_block(console: Console, iid: str, when: str, text: str) -> None:
    console.out(f"--- CLOCK {iid} {when} ---")
    for line in kv_lines(text):
        console.out(line)
    console.out("--- END CLOCK ---")


def context_block(console: Console, iid: str, ami: str, context: Dict[str, str]) -> None:
    console.out(f"--- CONTEXT {iid} ---")
    console.out(f"ami={ami}")
    for k, v in context.items():
        console.out(f"{k}={v}")
    console.out("--- END CONTEXT ---")


def mcast_block(console: Console, iid: str, az: str, relation: str, raw: str) -> None:
    doc = {"subscriber_id": iid, "az": az, "az_relation": relation}
    raw = raw.strip()
    if not raw:
        doc["error"] = "no result returned"
    else:
        try:
            doc["result"] = json.loads(raw)
        except ValueError:
            doc["error"] = "result is not valid JSON: " + raw[:200]
    console.out(f"--- MCAST {iid} ---")
    console.out(json.dumps(doc))
    console.out(f"--- END MCAST {iid} ---")


def result_ok(text: str) -> bool:
    """True when the text is one JSON object without an "error" key."""
    try:
        d = json.loads(text)
    except ValueError:
        return False
    return isinstance(d, dict) and bool(d) and not d.get("error")

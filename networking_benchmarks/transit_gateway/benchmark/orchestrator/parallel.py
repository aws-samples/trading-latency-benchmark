"""Run one worker per instance at the same time and show their output in instance order."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any, Callable, List, Sequence

from .util import Console


@dataclass
class Outcome:
    id: str
    ok: bool = True
    out: List[str] = field(default_factory=list)
    err: List[str] = field(default_factory=list)
    data: Any = None


def par_run(console: Console, fn: Callable[[str], Outcome], ids: Sequence[str]) -> List[Outcome]:
    """Call fn(id) for every id concurrently; print each outcome's lines in id order."""
    def guarded(i: str) -> Outcome:
        try:
            return fn(i)
        except Exception as e:  # a worker bug must fail the run, not vanish in a thread
            return Outcome(i, ok=False, err=[f"ERROR: {i}: {type(e).__name__}: {e}"])

    with ThreadPoolExecutor(max_workers=max(1, len(ids))) as pool:
        outcomes = list(pool.map(guarded, ids))
    for o in outcomes:
        for line in o.out:
            console.out(line)
        for line in o.err:
            console.err(line)
    return outcomes


def all_ok(outcomes: Sequence[Outcome]) -> bool:
    return all(o.ok for o in outcomes)

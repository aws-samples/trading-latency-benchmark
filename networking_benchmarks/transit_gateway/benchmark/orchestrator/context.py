"""Shared state for one invocation: configuration, console, transport and phase timing."""
from __future__ import annotations

import argparse
import time
from dataclasses import dataclass, field

from .transport import Transport
from .util import Console


@dataclass
class Ctx:
    cfg: argparse.Namespace
    console: Console
    transport: Transport
    bench_dir: str
    state_dir: str
    _t0: float = field(default_factory=time.monotonic)
    _calls0: int = 0

    def mark(self, name: str) -> None:
        """One '# timing' line per phase: seconds and remote calls since the previous mark."""
        now = time.monotonic()
        self.console.out(f"# timing: {name} took {int(now - self._t0)}s, "
                         f"{self.transport.calls - self._calls0} remote calls")
        self._t0, self._calls0 = now, self.transport.calls

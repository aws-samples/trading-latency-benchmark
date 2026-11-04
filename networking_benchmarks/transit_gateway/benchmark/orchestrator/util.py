"""Console output, sleeping, and the abort exception shared by every module."""
from __future__ import annotations

import os
import sys
import threading
import time
from typing import Optional, TextIO


class Abort(Exception):
    """Stop the run with an exit status; the reason has already been shown."""

    def __init__(self, rc: int = 1):
        super().__init__(rc)
        self.rc = rc


def sleep(secs: float) -> None:
    """time.sleep, except tests set TGW_BENCH_NO_SLEEP to skip waits."""
    if not os.environ.get("TGW_BENCH_NO_SLEEP"):
        time.sleep(secs)


class Console:
    """stdout and stderr plus an optional raw file holding everything shown.

    The raw file is the canonical run record that results.py parses.
    """

    def __init__(self, raw_path: Optional[str] = None):
        self._lock = threading.Lock()
        self._raw: Optional[TextIO] = open(raw_path, "w") if raw_path else None

    def _write(self, stream: TextIO, line: str) -> None:
        with self._lock:
            print(line, file=stream, flush=True)
            if self._raw:
                self._raw.write(line + "\n")
                self._raw.flush()

    def out(self, line: str = "") -> None:
        self._write(sys.stdout, line)

    def err(self, line: str = "") -> None:
        self._write(sys.stderr, line)

    def close(self) -> None:
        if self._raw:
            self._raw.close()
            self._raw = None

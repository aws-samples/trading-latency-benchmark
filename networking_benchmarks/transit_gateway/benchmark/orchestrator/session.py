"""Shared set-up for the commands that talk to a stack's instances without a manifest."""
from __future__ import annotations

from contextlib import contextmanager
from typing import Iterator

from . import cli
from .context import Ctx
from .model import Outputs, load_outputs, public_ips
from .prepare import bind_transport
from .transport import make_transport
from .util import Console


@contextmanager
def session(cfg) -> Iterator[Ctx]:
    """A context with an open transport; the transport is closed on exit."""
    console = Console()
    transport = make_transport(cfg.transport, console, cfg.ssh_user, cfg.ssh_key)
    try:
        yield Ctx(cfg, console, transport, cli.BENCH_DIR, getattr(cfg, "state_dir", ""))
    finally:
        transport.close()


def connect(ctx: Ctx) -> Outputs:
    """Read the stack outputs and make the transport able to reach every instance."""
    out = load_outputs(ctx.cfg.stack_name)
    public = public_ips(out.all_ids) if ctx.cfg.transport == "ssh" else None
    bind_transport(ctx, out.all_ids, public)
    return out

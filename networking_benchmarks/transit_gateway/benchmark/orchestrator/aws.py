"""Thin wrapper over the `aws` CLI.

The CLI (not boto3) keeps the operator's profile handling and lets tests substitute a fake.
"""
from __future__ import annotations

import subprocess


class AwsError(RuntimeError):
    pass


def aws(*args: str) -> str:
    """Run `aws <args>` and return stdout; raise AwsError on a non-zero exit."""
    p = subprocess.run(["aws", *args], capture_output=True, text=True)
    if p.returncode != 0:
        raise AwsError(f"aws {' '.join(args[:2])} failed: {p.stderr.strip()[:300]}")
    return p.stdout

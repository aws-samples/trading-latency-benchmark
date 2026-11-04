"""Orchestrator for the TGW multicast benchmark.

Three stages, cheapest last:
  1. EC2 user-data builds the instance environment once per boot (see cdk/stacks/main.py).
  2. prepare.py verifies and provisions each instance once and records the result in a manifest.
  3. run.py checks the manifest is still true, gates the clock, then runs the measurement.
"""

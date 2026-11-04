"""python3 -m orchestrator {run|prepare|check} [options]"""
import sys

from .check import check_main
from .run import main, prepare_main

COMMANDS = {"run": main, "prepare": prepare_main, "check": check_main}

if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
    print(f"usage: python3 -m orchestrator {{{'|'.join(COMMANDS)}}} [options]", file=sys.stderr)
    sys.exit(2)
sys.exit(COMMANDS[sys.argv[1]](sys.argv[2:]))

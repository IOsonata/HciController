#!/usr/bin/env python3
"""Compatibility entry point for the focused two-dongle CIS harness."""

from pathlib import Path
import runpy
import sys

_HARDWARE_DIR = Path(__file__).resolve().parents[2] / "hardware"
_TARGET = _HARDWARE_DIR / "hci_cis_pair_test.py"

# runpy executes the target as __main__ but does not make the target file's
# directory importable. The original focused hardware test imports sibling
# modules such as hci_commands, so preserve the environment it gets when run
# directly from tests/hardware.
if str(_HARDWARE_DIR) not in sys.path:
    sys.path.insert(0, str(_HARDWARE_DIR))

runpy.run_path(str(_TARGET), run_name="__main__")

"""Reusable BLE harness helpers."""

# Several migrated support modules predate the harness package and still use
# sibling imports such as `import hci_events`. Make the package directory an
# explicit import root so those modules remain self-contained under
# tests/harness/lib without reaching outside the harness.
from pathlib import Path
import sys

_LIB_DIR = str(Path(__file__).resolve().parent)
if _LIB_DIR not in sys.path:
    sys.path.insert(0, _LIB_DIR)

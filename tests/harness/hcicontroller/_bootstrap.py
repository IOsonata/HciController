"""Make the in-tree ``hcicontroller`` package importable by harness scripts."""

from pathlib import Path
import sys


_REPO_ROOT = Path(__file__).resolve().parents[3]
_PYTHON_DIR = _REPO_ROOT / "python"

if str(_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(_PYTHON_DIR))

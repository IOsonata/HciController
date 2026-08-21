#!/usr/bin/env python3
"""Official focused two-HciController CIS/ISO harness entry point."""

from pathlib import Path
import sys

_LIB_DIR = Path(__file__).resolve().parents[1] / "lib"
if str(_LIB_DIR) not in sys.path:
    sys.path.insert(0, str(_LIB_DIR))

import hci_cis_pair_test


if __name__ == "__main__":
    sys.exit(hci_cis_pair_test.main())

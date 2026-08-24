#!/usr/bin/env python3
"""Official focused two-HciController CIS/ISO harness entry point."""

import sys

import _bootstrap  # noqa: F401
from hcicontroller import hci_cis_pair_test


if __name__ == "__main__":
    sys.exit(hci_cis_pair_test.main())

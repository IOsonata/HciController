#!/usr/bin/env python3
"""Official HciController broad HCI/radio probe entry point."""

import sys

import _bootstrap  # noqa: F401
from hcicontroller import hci_ble_test


if __name__ == "__main__":
    sys.exit(hci_ble_test.main())

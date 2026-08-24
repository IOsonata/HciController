#!/usr/bin/env python3
"""Official two-HciController CIS/ISO test over native Bluetooth USB HCI."""

import sys

import _bootstrap  # noqa: F401
from hcicontroller import hci_cis_usb_pair_test


if __name__ == "__main__":
    sys.exit(hci_cis_usb_pair_test.main())

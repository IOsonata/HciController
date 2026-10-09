#!/usr/bin/env python3
"""Check HCI Reset after repeated native USB close/open cycles."""

import argparse
import sys

import _bootstrap  # noqa: F401
from hcicontroller.hci_ble_test import Hci
from hcicontroller.hci_transport import discover
from hcicontroller.pair_transport import bulk_spec


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--usb", help="native USB serial number or VID:PID")
    parser.add_argument("--count", type=int, default=20,
                        help="opens per alternate (default: 20)")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be positive")

    try:
        spec = discover("usb", usb_selector=args.usb)
        print("Controller:", spec, flush=True)
        for alt, selected in ((0, spec), (1, bulk_spec(spec))):
            for cycle in range(1, args.count + 1):
                hci = Hci(selected, raw=args.raw)
                try:
                    # One short event per session leaves the IN data toggle
                    # at DATA1. SET_INTERFACE on reopen must restore DATA0.
                    # Do not retry a lost completion: it is the regression.
                    hci.command(0x0C03)
                finally:
                    hci.close()
                print("PASS alt %d reopen %d/%d" %
                      (alt, cycle, args.count), flush=True)
    except Exception as err:
        print("FAIL:", err, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

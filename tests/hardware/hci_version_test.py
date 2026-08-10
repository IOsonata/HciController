#!/usr/bin/env python3
"""Verify the controller reports Bluetooth Core 5.4 through standard HCI."""

import argparse
import struct
import sys

from hci_ble_test import Hci, HciError, HciGone, find_port, status_text

OP_RESET = 0x0C03
OP_READ_LOCAL_VERSION_INFORMATION = 0x1001
CORE_5_4_VERSION = 0x0D


def main():
    parser = argparse.ArgumentParser(
        description="Read and validate the controller HCI/LMP version fields"
    )
    parser.add_argument("--port", help="HCI serial port")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    args = parser.parse_args()

    port = args.port or find_port()
    if port is None:
        print("No HCI serial port found. Use --port PORT.", file=sys.stderr)
        return 2

    hci = Hci(port, raw=args.raw)
    try:
        hci.command(OP_RESET)
        status, data = hci.command(
            OP_READ_LOCAL_VERSION_INFORMATION, allow_fail=True
        )
        if status != 0:
            raise HciError(
                "Read Local Version Information returned %s"
                % status_text(status)
            )

        if len(data) != 8:
            raise HciError(
                "Read Local Version Information returned %d data bytes, expected 8"
                % len(data)
            )

        hci_version, hci_revision, lmp_version, company_id, lmp_subversion = \
            struct.unpack("<BHBHH", data)

        print("Controller version report")
        print("   HCI version      0x%02X" % hci_version)
        print("   HCI revision     0x%04X" % hci_revision)
        print("   LMP/PAL version  0x%02X" % lmp_version)
        print("   Company ID       0x%04X" % company_id)
        print("   LMP/PAL subver   0x%04X" % lmp_subversion)

        errors = []
        if hci_version != CORE_5_4_VERSION:
            errors.append(
                "HCI version is 0x%02X, Core 5.4 requires version value 0x%02X"
                % (hci_version, CORE_5_4_VERSION)
            )
        if lmp_version != CORE_5_4_VERSION:
            errors.append(
                "LMP/PAL version is 0x%02X, expected Core 5.4 value 0x%02X"
                % (lmp_version, CORE_5_4_VERSION)
            )

        if errors:
            for error in errors:
                print("FAIL: %s" % error, file=sys.stderr)
            return 1

        print("[ok] controller reports Bluetooth Core 5.4 (0x0D)")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        hci.close()


if __name__ == "__main__":
    sys.exit(main())

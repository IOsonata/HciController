#!/usr/bin/env python3
"""Verify the controller reports the Bluetooth Core version expected by the target."""

import argparse
import struct
import sys

from hci_ble_test import Hci, HciError, HciGone, find_port, status_text

OP_RESET = 0x0C03
OP_READ_LOCAL_VERSION_INFORMATION = 0x1001

# Bluetooth SIG Assigned Numbers, Core specification versions.
CORE_VERSIONS = {
    "5.4": 0x0D,
    "6.0": 0x0E,
}


def main():
    parser = argparse.ArgumentParser(
        description="Read and validate the controller HCI/LMP version fields"
    )
    parser.add_argument("--port", help="HCI serial port")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    parser.add_argument(
        "--core",
        choices=sorted(CORE_VERSIONS),
        default="5.4",
        help="Bluetooth Core version expected from this target (default: 5.4)",
    )
    args = parser.parse_args()

    expected_version = CORE_VERSIONS[args.core]

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
        print("   expected Core    %s (0x%02X)" % (args.core, expected_version))
        print("   HCI version      0x%02X" % hci_version)
        print("   HCI revision     0x%04X" % hci_revision)
        print("   LMP/PAL version  0x%02X" % lmp_version)
        print("   Company ID       0x%04X" % company_id)
        print("   LMP/PAL subver   0x%04X" % lmp_subversion)

        errors = []
        if hci_version != expected_version:
            errors.append(
                "HCI version is 0x%02X, Core %s requires version value 0x%02X"
                % (hci_version, args.core, expected_version)
            )
        if lmp_version != expected_version:
            errors.append(
                "LMP/PAL version is 0x%02X, expected Core %s value 0x%02X"
                % (lmp_version, args.core, expected_version)
            )

        if errors:
            for error in errors:
                print("FAIL: %s" % error, file=sys.stderr)
            return 1

        print(
            "[ok] controller reports Bluetooth Core %s (0x%02X)"
            % (args.core, expected_version)
        )
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        hci.close()


if __name__ == "__main__":
    sys.exit(main())

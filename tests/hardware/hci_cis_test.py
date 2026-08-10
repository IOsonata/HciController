#!/usr/bin/env python3
"""Focused no-peer CIS capability check for HciController.

This is the first hardware gate for connected isochronous streams. It does not
try to establish a CIS over the air; that needs a second controller. It checks
the two local facts that must agree before that test is worth doing:

  * LE Set Host Feature accepts bit 32 through both command forms dispatched by
    this firmware.
  * LE Set CIG Parameters accepts a valid one-CIS group and returns a CIS
    connection handle.

Run from this directory or from anywhere in the repository:

    python3 tests/hardware/hci_cis_test.py
    python3 tests/hardware/hci_cis_test.py --port /dev/cu.usbmodem142301
"""

import argparse
import struct
import sys

import hci_commands
from hci_ble_test import ERROR_NAMES, Hci, HciError, HciGone, find_port


OP_LE_SET_HOST_FEATURE = 0x2074
OP_LE_SET_HOST_FEATURE_V2 = 0x2097
OP_LE_SET_CIG_PARAMS = 0x2062
OP_LE_REMOVE_CIG = 0x2065

CIS_HOST_SUPPORT_BIT = 32


def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))


def print_result(label, status):
    print("%-34s %s" % (label + ":", status_text(status)))


def set_host_feature_v1(hci, enabled):
    payload = bytes([CIS_HOST_SUPPORT_BIT, 1 if enabled else 0])
    return hci.command(OP_LE_SET_HOST_FEATURE, payload, allow_fail=True)[0]


def set_host_feature_v2(hci, enabled):
    payload = struct.pack("<HB", CIS_HOST_SUPPORT_BIT, 1 if enabled else 0)
    return hci.command(OP_LE_SET_HOST_FEATURE_V2, payload, allow_fail=True)[0]


def parse_cig_return(data):
    if len(data) < 2:
        raise HciError("LE Set CIG Parameters returned only %d data byte(s)"
                       % len(data))

    cig_id = data[0]
    cis_count = data[1]
    expected = 2 + cis_count * 2
    if len(data) != expected:
        raise HciError(
            "LE Set CIG Parameters says %d CIS but returned %d data byte(s), "
            "expected %d" % (cis_count, len(data), expected))

    handles = [struct.unpack("<H", data[2 + i * 2:4 + i * 2])[0] & 0x0FFF
               for i in range(cis_count)]
    return cig_id, handles


def main():
    parser = argparse.ArgumentParser(
        description="Check local CIS Host Support and CIG creation")
    parser.add_argument("--port", help="HCI CDC serial port; auto-detected if omitted")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No HciController serial port found. Use --port.", file=sys.stderr)
        return 2

    print("CIS local capability check on %s" % port)
    hci = Hci(port, raw=args.raw)
    cig_created = False
    created_cig_id = None

    try:
        hci.setup()

        # Test the legacy one-octet feature-number form independently.
        v1 = set_host_feature_v1(hci, True)
        print_result("LE Set Host Feature bit 32", v1)
        if v1 == 0:
            clear = set_host_feature_v1(hci, False)
            if clear != 0:
                print_result("clear Host Feature v1 bit 32", clear)

        # Test the v2 command as its own advertised/dispatched capability. Keep
        # bit 32 set when it succeeds so the CIG test runs with host support on.
        v2 = set_host_feature_v2(hci, True)
        print_result("LE Set Host Feature v2 bit 32", v2)

        # If v2 is unavailable but v1 worked, restore bit 32 through v1 before
        # creating the CIG. The CIG result is reported independently either way.
        feature_enabled = v2 == 0
        if not feature_enabled and v1 == 0:
            restore = set_host_feature_v1(hci, True)
            print_result("restore Host Feature v1 bit 32", restore)
            feature_enabled = restore == 0

        cig_cmd = hci_commands.BY_OPCODE[OP_LE_SET_CIG_PARAMS]
        cig_status, cig_data = hci.command(
            OP_LE_SET_CIG_PARAMS, cig_cmd.build(None), allow_fail=True)
        print_result("LE Set CIG Parameters", cig_status)

        handles = []
        if cig_status == 0:
            created_cig_id, handles = parse_cig_return(cig_data)
            cig_created = True
            print("CIG 0x%02X returned %d CIS handle(s): %s" % (
                created_cig_id, len(handles),
                ", ".join("0x%04X" % handle for handle in handles)))

        ok = v1 == 0 and v2 == 0 and cig_status == 0 and len(handles) == 1

        print()
        if ok:
            print("PASS: local CIS Host Support and one-CIS CIG creation agree.")
            print("Next gate: establish that CIS with a real peer and move ISO data.")
            return 0

        print("FAIL: the controller's local CIS capabilities are inconsistent.")
        if v1 != 0 or v2 != 0:
            print("Host Feature bit 32 is not accepted through every command form "
                  "the firmware dispatches.")
        if cig_status != 0:
            print("CIG creation is not accepted with the configured CIS resources.")
        elif len(handles) != 1:
            print("CIG creation succeeded but did not return the one CIS handle requested.")
        if cig_status == 0 and not feature_enabled:
            print("CIG creation succeeds even though Host Feature bit 32 could not be enabled.")
        return 1

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        # Remove any group we created before clearing host support. The remove
        # payload is just the CIG identifier returned by Set CIG Parameters.
        if cig_created and created_cig_id is not None:
            try:
                hci.command(OP_LE_REMOVE_CIG, bytes([created_cig_id]),
                            allow_fail=True)
            except (HciError, HciGone):
                pass
        try:
            set_host_feature_v2(hci, False)
            set_host_feature_v1(hci, False)
        except (HciError, HciGone):
            pass
        hci.close()


if __name__ == "__main__":
    sys.exit(main())

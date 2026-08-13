#!/usr/bin/env python3
"""Two-dongle base validation for the HciController harness."""

import argparse
from pathlib import Path
import sys

_HARNESS_DIR = Path(__file__).resolve().parents[1]
if str(_HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(_HARNESS_DIR))

from lib.hci_pair import (
    Hci,
    HciError,
    HciGone,
    disconnect_acl_pair,
    establish_legacy_acl_pair,
    resolve_pair_ports,
)
from lib.profile import read_controller_capabilities
from lib.results import ResultBook

EXPECTED_CORE_VERSION = 0x10
EXPECTED_STATES = bytes.fromhex("ff ff ff ff ff 03 00 00")


def check_profile(book, label, capabilities):
    version = capabilities["version"]
    commands = capabilities["commands"]
    features = capabilities["features"]
    states = capabilities["states"]

    if len(version) >= 8 and version[0] == EXPECTED_CORE_VERSION and version[3] == EXPECTED_CORE_VERSION:
        book.passed("Profile", "%s Core version" % label, "6.2")
    else:
        book.failed("Profile", "%s Core version" % label, version.hex(" "))

    if len(commands) == 64:
        book.passed("Profile", "%s Supported Commands" % label, "64 octets")
    else:
        book.failed("Profile", "%s Supported Commands" % label, "%d octets" % len(commands))

    if len(features) == 8:
        book.passed("Profile", "%s LE features" % label, features.hex(" "))
    else:
        book.failed("Profile", "%s LE features" % label, "%d octets" % len(features))

    if states == EXPECTED_STATES:
        book.passed("Profile", "%s LE Supported States" % label)
    else:
        detail = "unavailable" if states is None else states.hex(" ")
        book.failed("Profile", "%s LE Supported States" % label, detail)

    if capabilities["all_local_features"]:
        book.passed("Profile", "%s extended feature pages" % label)
    else:
        book.failed("Profile", "%s extended feature pages" % label)

    minimum = capabilities["minimum_connection_interval"]
    if minimum and len(minimum) >= 2 and len(minimum) == 2 + minimum[1] * 6:
        book.passed(
            "Profile",
            "%s minimum connection interval" % label,
            "%u, %u group(s)" % (minimum[0], minimum[1]),
        )
    else:
        book.failed("Profile", "%s minimum connection interval" % label)


def run_acl_phase(book, label, central, peripheral):
    handles = None
    try:
        info = establish_legacy_acl_pair(central, peripheral)
        handles = (info["central_handle"], info["peripheral_handle"])
        book.passed(
            "ACL roles",
            label,
            "central 0x%04X / peripheral 0x%04X" % handles,
        )
    except (HciError, HciGone) as err:
        book.failed("ACL roles", label, str(err))
    finally:
        if handles is not None:
            disconnect_acl_pair(
                central, peripheral, handles[0], handles[1]
            )


def main():
    parser = argparse.ArgumentParser(
        description="Validate two HciController dongles and both ACL role assignments"
    )
    parser.add_argument("--a", help="first HciController serial port")
    parser.add_argument("--b", help="second HciController serial port")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    args = parser.parse_args()

    book = ResultBook("HciController two-dongle base validation")

    try:
        port_a, port_b = resolve_pair_ports(args.a, args.b)
    except HciError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    print("Dongle A: %s" % port_a)
    print("Dongle B: %s" % port_b)

    a = Hci(port_a, raw=args.raw)
    b = Hci(port_b, raw=args.raw)
    try:
        try:
            check_profile(book, "A", read_controller_capabilities(a))
        except Exception as err:
            book.failed("Profile", "A capability read", str(err))

        try:
            check_profile(book, "B", read_controller_capabilities(b))
        except Exception as err:
            book.failed("Profile", "B capability read", str(err))

        run_acl_phase(book, "A Central -> B Peripheral", a, b)
        run_acl_phase(book, "B Central -> A Peripheral", b, a)

    finally:
        a.close()
        b.close()

    book.print_report()
    return book.exit_code()


if __name__ == "__main__":
    sys.exit(main())

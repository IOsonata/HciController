#!/usr/bin/env python3
"""Two-dongle base validation for the HciController harness."""

import argparse
from pathlib import Path
import struct
import sys

_HARNESS_DIR = Path(__file__).resolve().parents[1]
if str(_HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(_HARNESS_DIR))

from lib.hci_core_conditions import (
    EXPECTED_LE_SUPPORTED_STATES,
    FEAT_CIS_PERIPHERAL,
    feature,
    require_command_bit,
    validate_conditional_commands,
)
from lib.hci_pair import (
    Hci,
    HciError,
    HciGone,
    disconnect_acl_pair,
    establish_legacy_acl_pair,
)
from lib.hci_transport import SelectionError
from lib.pair_transport import resolve_pair
from lib.profile import read_controller_capabilities
from lib.results import ResultBook

EXPECTED_CORE_VERSION = 0x10


def check_profile(book, label, capabilities):
    version = capabilities["version"]
    commands = capabilities["commands"]
    features = capabilities["features"]
    states = capabilities["states"]

    if (len(version) >= 8
            and version[0] == EXPECTED_CORE_VERSION
            and version[3] == EXPECTED_CORE_VERSION):
        book.passed("Profile", "%s Core version" % label, "6.2")
    else:
        book.failed("Profile", "%s Core version" % label, version.hex(" "))

    if len(commands) == 64:
        book.passed("Profile", "%s Supported Commands" % label, "64 octets")
    else:
        book.failed("Profile", "%s Supported Commands" % label,
                    "%d octets" % len(commands))

    if len(features) == 8:
        book.passed("Profile", "%s LE features" % label, features.hex(" "))
    else:
        book.failed("Profile", "%s LE features" % label,
                    "%d octets" % len(features))

    errors = []
    if len(commands) == 64 and len(features) == 8:
        validate_conditional_commands(commands, features, errors)

        # Core 6.0 Extended Feature Set commands enabled by this release.
        for octet, bit, opcode, name in (
            (47, 2, 0x2087, "LE Read All Local Supported Features"),
            (47, 3, 0x2088, "LE Read All Remote Features"),
            (47, 4, 0x2097, "LE Set Host Feature v2"),
        ):
            require_command_bit(commands, octet, bit, opcode, name, errors,
                                "nRF52840 SDC Core 6.0")

        # Core 6.2 commands enabled by the current nRF52 multirole profile.
        for octet, bit, opcode, name in (
            (48, 1, 0x209D, "LE Frame Space Update"),
            (48, 5, 0x20A1, "LE Connection Rate Request"),
            (48, 6, 0x20A2, "LE Set Default Rate Parameters"),
            (48, 7, 0x20A3, "LE Read Minimum Supported Connection Interval"),
        ):
            require_command_bit(commands, octet, bit, opcode, name, errors,
                                "nRF52840 SDC Core 6.2")

        require_command_bit(commands, 28, 3, 0x201C,
                            "LE Read Supported States", errors)

        if feature(features, FEAT_CIS_PERIPHERAL):
            require_command_bit(
                commands, 7, 2, 0x0C15,
                "Read Connection Accept Timeout", errors,
                "CIS Peripheral timeout configuration",
            )
            require_command_bit(
                commands, 7, 3, 0x0C16,
                "Write Connection Accept Timeout", errors,
                "CIS Peripheral timeout configuration",
            )

    if errors:
        book.failed("Profile", "%s conditional commands" % label,
                    "; ".join(errors))
    elif len(commands) == 64 and len(features) == 8:
        book.passed("Profile", "%s conditional commands" % label)

    if states == EXPECTED_LE_SUPPORTED_STATES:
        book.passed("Profile", "%s LE Supported States" % label)
    else:
        detail = "unavailable" if states is None else states.hex(" ")
        book.failed("Profile", "%s LE Supported States" % label, detail)

    all_local = capabilities["all_local_features"]
    if all_local is not None and len(all_local) == 249:
        book.passed("Profile", "%s extended feature pages" % label,
                    "249 byte return")
    else:
        detail = "unavailable" if all_local is None else "%d bytes" % len(all_local)
        book.failed("Profile", "%s extended feature pages" % label, detail)

    minimum = capabilities["minimum_connection_interval"]
    if minimum and len(minimum) >= 2 and len(minimum) == 2 + minimum[1] * 6:
        book.passed(
            "Profile",
            "%s minimum connection interval" % label,
            "%u, %u group(s)" % (minimum[0], minimum[1]),
        )
    else:
        book.failed("Profile", "%s minimum connection interval" % label)

    if len(features) == 8 and feature(features, FEAT_CIS_PERIPHERAL):
        timeout = capabilities["connection_accept_timeout"]
        if timeout is not None and len(timeout) == 2:
            book.passed(
                "Profile", "%s CIS connection accept timeout" % label,
                "%u slots; read and write-back passed"
                % struct.unpack("<H", timeout)[0],
            )
        else:
            book.failed(
                "Profile", "%s CIS connection accept timeout" % label,
                "Read/Write Connection Accept Timeout did not complete",
            )


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
    parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto",
        help="host transport; auto prefers native USB when present",
    )
    parser.add_argument(
        "--a",
        help="first controller selector: serial device or native USB serial number",
    )
    parser.add_argument(
        "--b",
        help="second controller selector: serial device or native USB serial number",
    )
    parser.add_argument("--raw", action="store_true", help="show raw HCI traffic")
    args = parser.parse_args()

    book = ResultBook("HciController two-dongle base validation")

    try:
        spec_a, spec_b = resolve_pair(args.a, args.b, kind=args.transport)
    except (HciError, SelectionError) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    print("Dongle A: %s" % spec_a)
    print("Dongle B: %s" % spec_b)

    a = Hci(spec_a, raw=args.raw)
    b = Hci(spec_b, raw=args.raw)
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

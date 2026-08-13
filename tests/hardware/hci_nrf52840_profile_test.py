#!/usr/bin/env python3
"""Validate the nRF52840 + sdk-nrfxlib HEAD HCI release profile."""

import argparse
import struct
import sys

from hci_ble_test import Hci, HciError, HciGone, find_port, status_text
from hci_core_conditions import (
    EXPECTED_LE_SUPPORTED_STATES,
    FEAT_CIS_PERIPHERAL,
    feature,
    require_command_bit,
    validate_conditional_commands,
)

OP_RESET = 0x0C03
OP_READ_LOCAL_VERSION = 0x1001
OP_READ_LOCAL_SUPPORTED_COMMANDS = 0x1002
OP_LE_READ_LOCAL_SUPPORTED_FEATURES = 0x2003
OP_READ_CONN_ACCEPT_TIMEOUT = 0x0C15
OP_WRITE_CONN_ACCEPT_TIMEOUT = 0x0C16
OP_LE_READ_SUPPORTED_STATES = 0x201C
OP_LE_READ_ALL_LOCAL_SUPPORTED_FEATURES = 0x2087
OP_LE_READ_MIN_SUPPORTED_CONN_INTERVAL = 0x20A3

# command_coverage.py reads these without importing the hardware module.
COVERED_OPCODES = {0x0C15, 0x0C16, 0x201C}
EXCLUDED_OPCODES = set()

EXPECTED_CORE_VERSION = 0x10  # Bluetooth Core 6.2


def command_bit(commands, octet, bit):
    return bool(commands[octet] & (1 << bit))


def main():
    parser = argparse.ArgumentParser(
        description="Validate the nRF52840 + SDC HEAD HCI profile"
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
        errors = []

        status, version = hci.command(OP_READ_LOCAL_VERSION, allow_fail=True)
        if status != 0:
            errors.append("Read Local Version returned %s" % status_text(status))
        elif len(version) < 8:
            errors.append("Read Local Version returned %d bytes, expected 8" % len(version))
        else:
            if version[0] != EXPECTED_CORE_VERSION:
                errors.append(
                    "HCI version is 0x%02X, expected Core 6.2 (0x10)" % version[0]
                )
            if version[3] != EXPECTED_CORE_VERSION:
                errors.append(
                    "LMP/PAL version is 0x%02X, expected Core 6.2 (0x10)" % version[3]
                )

        status, commands = hci.command(
            OP_READ_LOCAL_SUPPORTED_COMMANDS, allow_fail=True
        )
        if status != 0:
            raise HciError(
                "Read Local Supported Commands returned %s" % status_text(status)
            )
        if len(commands) != 64:
            raise HciError(
                "Read Local Supported Commands returned %d bytes, expected 64"
                % len(commands)
            )

        status, features = hci.command(
            OP_LE_READ_LOCAL_SUPPORTED_FEATURES, allow_fail=True
        )
        if status != 0:
            raise HciError(
                "LE Read Local Supported Features returned %s"
                % status_text(status)
            )
        if len(features) != 8:
            raise HciError(
                "LE Read Local Supported Features returned %d bytes, expected 8"
                % len(features)
            )

        # Everything activated by the Bluetooth 5.4 feature bits remains
        # mandatory in the 6.2 release profile.
        validate_conditional_commands(commands, features, errors)

        # Core 6.0 Extended Feature Set surface supplied by the current nRF52
        # multirole SDC.
        for octet, bit, opcode, name in (
            (47, 2, 0x2087, "LE Read All Local Supported Features"),
            (47, 3, 0x2088, "LE Read All Remote Features"),
            (47, 4, 0x2097, "LE Set Host Feature v2"),
        ):
            require_command_bit(commands, octet, bit, opcode, name, errors,
                                "nRF52840 SDC Core 6.0")

        # Core 6.2 capabilities enabled by the nRF52 multirole SDC profile.
        for octet, bit, opcode, name in (
            (48, 1, 0x209D, "LE Frame Space Update"),
            (48, 5, 0x20A1, "LE Connection Rate Request"),
            (48, 6, 0x20A2, "LE Set Default Rate Parameters"),
            (48, 7, 0x20A3, "LE Read Minimum Supported Connection Interval"),
        ):
            require_command_bit(commands, octet, bit, opcode, name, errors,
                                "nRF52840 SDC Core 6.2")

        # The all-local-features read is harmless and proves the 6.0 command is
        # not merely a Supported Commands bit patched onto the wire.
        status, all_local = hci.command(
            OP_LE_READ_ALL_LOCAL_SUPPORTED_FEATURES, allow_fail=True
        )
        if status != 0:
            errors.append(
                "LE Read All Local Supported Features returned %s"
                % status_text(status)
            )
        elif not all_local:
            errors.append("LE Read All Local Supported Features returned no data")

        # Read Minimum Supported Connection Interval is likewise read-only and
        # has a variable response: two octets followed by six octets per group.
        status, minimum = hci.command(
            OP_LE_READ_MIN_SUPPORTED_CONN_INTERVAL, allow_fail=True
        )
        if status != 0:
            errors.append(
                "LE Read Minimum Supported Connection Interval returned %s"
                % status_text(status)
            )
        elif len(minimum) < 2:
            errors.append(
                "LE Read Minimum Supported Connection Interval returned %d bytes"
                % len(minimum)
            )
        else:
            groups = minimum[1]
            expected = 2 + groups * 6
            if groups > 41:
                errors.append(
                    "LE Read Minimum Supported Connection Interval returned %d groups, max 41"
                    % groups
                )
            elif len(minimum) != expected:
                errors.append(
                    "LE Read Minimum Supported Connection Interval returned %d bytes for %d groups, expected %d"
                    % (len(minimum), groups, expected)
                )
            else:
                print(
                    "[ok] minimum connection interval %u, %u interval group(s)"
                    % (minimum[0], groups)
                )

        if feature(features, FEAT_CIS_PERIPHERAL):
            require_command_bit(
                commands, 7, 2, OP_READ_CONN_ACCEPT_TIMEOUT,
                "Read Connection Accept Timeout", errors,
                "CIS Peripheral timeout configuration"
            )
            require_command_bit(
                commands, 7, 3, OP_WRITE_CONN_ACCEPT_TIMEOUT,
                "Write Connection Accept Timeout", errors,
                "CIS Peripheral timeout configuration"
            )

        status, timeout_data = hci.command(
            OP_READ_CONN_ACCEPT_TIMEOUT, allow_fail=True
        )
        if status != 0:
            errors.append(
                "Read Connection Accept Timeout returned %s" % status_text(status)
            )
        elif len(timeout_data) != 2:
            errors.append(
                "Read Connection Accept Timeout returned %d bytes, expected 2"
                % len(timeout_data)
            )
        else:
            timeout = struct.unpack("<H", timeout_data)[0]
            status, write_data = hci.command(
                OP_WRITE_CONN_ACCEPT_TIMEOUT,
                struct.pack("<H", timeout),
                allow_fail=True,
            )
            if status != 0:
                errors.append(
                    "Write Connection Accept Timeout returned %s"
                    % status_text(status)
                )
            elif write_data:
                errors.append(
                    "Write Connection Accept Timeout returned %d unexpected bytes"
                    % len(write_data)
                )

        require_command_bit(
            commands, 28, 3, OP_LE_READ_SUPPORTED_STATES,
            "LE Read Supported States", errors
        )
        status, states = hci.command(
            OP_LE_READ_SUPPORTED_STATES, allow_fail=True
        )
        if status != 0:
            errors.append(
                "LE Read Supported States returned %s" % status_text(status)
            )
        elif states != EXPECTED_LE_SUPPORTED_STATES:
            errors.append(
                "LE Read Supported States is %s, expected %s"
                % (states.hex(" "), EXPECTED_LE_SUPPORTED_STATES.hex(" "))
            )

        if errors:
            for error in errors:
                print("FAIL: %s" % error, file=sys.stderr)
            return 1

        print("[ok] nRF52840 + SDC HEAD Core 6.2 profile checks passed")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        hci.close()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Validate the nRF52-facing HCI surface against active Core 5.4 conditions."""

import argparse
import struct
import sys

from hci_ble_test import Hci, HciError, HciGone, find_port, status_text

OP_RESET = 0x0C03
OP_READ_LOCAL_SUPPORTED_COMMANDS = 0x1002
OP_LE_READ_LOCAL_SUPPORTED_FEATURES = 0x2003
OP_READ_CONN_ACCEPT_TIMEOUT = 0x0C15
OP_WRITE_CONN_ACCEPT_TIMEOUT = 0x0C16
OP_LE_READ_SUPPORTED_STATES = 0x201C

OP_LE_REMOTE_CONN_PARAM_REQ_REPLY = 0x2020
OP_LE_REMOTE_CONN_PARAM_REQ_NEG_REPLY = 0x2021

# Core 6.0 commands which must not leak through the nRF52 Core 5.4 profile.
CORE_6_ONLY_OPCODES = (0x2087, 0x2088, 0x2097)

# command_coverage.py parses these literals without importing this module, so
# pyserial is not required by the host-side coverage check.
COVERED_OPCODES = {0x0C15, 0x0C16, 0x201C}
EXCLUDED_OPCODES = {0x2087, 0x2088, 0x2097}


def command_bit(commands, octet, bit):
    return bool(commands[octet] & (1 << bit))


def require_command_bit(commands, octet, bit, opcode, name, errors):
    if not command_bit(commands, octet, bit):
        errors.append(
            "%s (0x%04X) missing from Supported Commands octet %d bit %d"
            % (name, opcode, octet, bit)
        )


def main():
    parser = argparse.ArgumentParser(
        description="Validate the active Bluetooth Core 5.4 HCI profile"
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

        status, commands = hci.command(
            OP_READ_LOCAL_SUPPORTED_COMMANDS, allow_fail=True
        )
        if status != 0:
            raise HciError(
                "Read Local Supported Commands returned %s" % status_text(status)
            )
        if len(commands) != 64:
            raise HciError(
                "Read Local Supported Commands returned %d bytes, Core 5.4 "
                "requires the 64-octet bitmap" % len(commands)
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

        errors = []

        # Core 5.4 C.40 is active because this build enables CIS Peripheral.
        require_command_bit(
            commands, 7, 2, OP_READ_CONN_ACCEPT_TIMEOUT,
            "Read Connection Accept Timeout", errors
        )
        require_command_bit(
            commands, 7, 3, OP_WRITE_CONN_ACCEPT_TIMEOUT,
            "Write Connection Accept Timeout", errors
        )

        status, timeout_data = hci.command(
            OP_READ_CONN_ACCEPT_TIMEOUT, allow_fail=True
        )
        if status != 0:
            errors.append(
                "Read Connection Accept Timeout returned %s"
                % status_text(status)
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
                    "Write Connection Accept Timeout returned %d unexpected "
                    "data bytes" % len(write_data)
                )
            else:
                print(
                    "[ok] C.40 Connection Accept Timeout read/write: 0x%04X"
                    % timeout
                )

        # Mandatory LE Read Supported States compatibility implementation.
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
        elif len(states) != 8:
            errors.append(
                "LE Read Supported States returned %d bytes, expected 8"
                % len(states)
            )
        else:
            print("[ok] LE Read Supported States:", states.hex(" "))

        # Core 5.4 C.6 is conditional on the controller advertising the
        # Connection Parameters Request Procedure (LE feature bit 1).
        conn_param_request = bool(features[0] & (1 << 1))
        if conn_param_request:
            print(
                "C.6 active: Connection Parameters Request Procedure is "
                "advertised"
            )
            require_command_bit(
                commands, 33, 4, OP_LE_REMOTE_CONN_PARAM_REQ_REPLY,
                "LE Remote Connection Parameter Request Reply", errors
            )
            require_command_bit(
                commands, 33, 5, OP_LE_REMOTE_CONN_PARAM_REQ_NEG_REPLY,
                "LE Remote Connection Parameter Request Negative Reply", errors
            )
        else:
            print(
                "[ok] C.6 inactive: Connection Parameters Request Procedure "
                "not advertised"
            )

        # A 5.4-facing controller must not advertise the Core 6.0 extended
        # feature-set HCI commands through the 5.4 64-octet bitmap.
        for bit, opcode in zip((2, 3, 4), CORE_6_ONLY_OPCODES):
            if command_bit(commands, 47, bit):
                errors.append(
                    "Core 6.0 opcode 0x%04X still advertised in octet 47 bit %d"
                    % (opcode, bit)
                )

        # And direct attempts must not fall through to the newer SDC backend.
        for opcode in CORE_6_ONLY_OPCODES:
            status, _ = hci.command(opcode, allow_fail=True)
            if status != 0x01:
                errors.append(
                    "Core 6.0 opcode 0x%04X returned %s, expected Unknown HCI "
                    "Command (0x01)"
                    % (opcode, status_text(status))
                )

        if errors:
            for error in errors:
                print("FAIL: %s" % error, file=sys.stderr)
            return 1

        print("[ok] Core 5.4 command/profile checks passed")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        hci.close()


if __name__ == "__main__":
    sys.exit(main())

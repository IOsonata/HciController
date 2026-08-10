#!/usr/bin/env python3
"""Validate the nRF52-facing HCI surface against active Core 5.4 conditions.

This is deliberately independent of the C++ dispatch table and of
hci_commands.py.  The Controller's own LE feature bitmap activates the Core
5.4 conditional requirements below; the Supported Commands bitmap then has to
contain every command those active conditions make mandatory.
"""

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

# Core 6.0 commands which must not leak through the nRF52 Core 5.4 profile.
CORE_6_ONLY_OPCODES = (0x2087, 0x2088, 0x2097)

# command_coverage.py parses these literals without importing this module, so
# pyserial is not required by the host-side coverage check.  Only commands
# driven specifically by this file belong here; the conditional commands below
# are also present in hci_commands.py and are exercised by the broad probe.
COVERED_OPCODES = {0x0C15, 0x0C16, 0x201C}
EXCLUDED_OPCODES = {0x2087, 0x2088, 0x2097}

EXPECTED_LE_SUPPORTED_STATES = bytes.fromhex("ff ff ff ff ff 03 00 00")

# LE FeatureSet bit numbers, Core 5.4 Vol 6 Part B Table 4.7.
FEAT_CONN_PARAM_REQ = 1
FEAT_SCA_UPDATES = 26
FEAT_CIS_CENTRAL = 28
FEAT_CIS_PERIPHERAL = 29
FEAT_ISO_BROADCASTER = 30
FEAT_SYNC_RECEIVER = 31
FEAT_POWER_CONTROL_1 = 33
FEAT_POWER_CONTROL_2 = 34
FEAT_PATH_LOSS = 35
FEAT_CONN_SUBRATING = 37
FEAT_CHANNEL_CLASSIFICATION = 39
FEAT_ADV_CODING_SELECTION = 40
FEAT_PAWR_ADVERTISER = 43
FEAT_PAWR_SCANNER = 44


def command_bit(commands, octet, bit):
    return bool(commands[octet] & (1 << bit))


def feature(features, bit):
    return bool(features[bit // 8] & (1 << (bit % 8)))


def require_command_bit(commands, octet, bit, opcode, name, errors, condition=None):
    if command_bit(commands, octet, bit):
        return
    prefix = (condition + ": ") if condition else ""
    errors.append(
        "%s%s (0x%04X) missing from Supported Commands octet %d bit %d"
        % (prefix, name, opcode, octet, bit)
    )


def require_group(commands, condition, rows, errors):
    for octet, bit, opcode, name in rows:
        require_command_bit(commands, octet, bit, opcode, name, errors, condition)


def validate_conditional_commands(commands, features, errors):
    """Evaluate Core 5.4 conditions that are driven directly by LE features."""

    conn_param = feature(features, FEAT_CONN_PARAM_REQ)
    sca = feature(features, FEAT_SCA_UPDATES)
    cis_central = feature(features, FEAT_CIS_CENTRAL)
    cis_peripheral = feature(features, FEAT_CIS_PERIPHERAL)
    iso_broadcaster = feature(features, FEAT_ISO_BROADCASTER)
    sync_receiver = feature(features, FEAT_SYNC_RECEIVER)
    power1 = feature(features, FEAT_POWER_CONTROL_1)
    power2 = feature(features, FEAT_POWER_CONTROL_2)
    path_loss = feature(features, FEAT_PATH_LOSS)
    subrating = feature(features, FEAT_CONN_SUBRATING)
    channel_class = feature(features, FEAT_CHANNEL_CLASSIFICATION)
    adv_coding = feature(features, FEAT_ADV_CODING_SELECTION)
    pawr_adv = feature(features, FEAT_PAWR_ADVERTISER)
    pawr_scan = feature(features, FEAT_PAWR_SCANNER)

    # Vol 6 Part B Table 4.7 requires the two LE Power Control Request bits to
    # have the same value.  A mismatch means the feature report itself is bad
    # before any HCI command requirement is considered.
    if power1 != power2:
        errors.append(
            "LE Power Control Request feature bits 33 and 34 disagree"
        )
    power_control = power1 and power2

    if conn_param:
        require_group(commands, "C.6", (
            (33, 4, 0x2020, "LE Remote Connection Parameter Request Reply"),
            (33, 5, 0x2021, "LE Remote Connection Parameter Request Negative Reply"),
        ), errors)

    if cis_central:
        require_group(commands, "C.39 CIS Central", (
            (41, 7, 0x2062, "LE Set CIG Parameters"),
            (42, 0, 0x2063, "LE Set CIG Parameters Test"),
            (42, 1, 0x2064, "LE Create CIS"),
            (42, 2, 0x2065, "LE Remove CIG"),
        ), errors)

    if cis_peripheral:
        require_group(commands, "C.40 CIS Peripheral", (
            (42, 3, 0x2066, "LE Accept CIS Request"),
            (42, 4, 0x2067, "LE Reject CIS Request"),
        ), errors)

    if iso_broadcaster:
        require_group(commands, "C.41 Isochronous Broadcaster", (
            (42, 5, 0x2068, "LE Create BIG"),
            (42, 6, 0x2069, "LE Create BIG Test"),
            (42, 7, 0x206A, "LE Terminate BIG"),
        ), errors)

    if sync_receiver:
        require_group(commands, "C.42 Synchronized Receiver", (
            (43, 0, 0x206B, "LE BIG Create Sync"),
            (43, 1, 0x206C, "LE BIG Terminate Sync"),
        ), errors)

    if sca and (cis_central or cis_peripheral):
        require_group(commands, "C.44 SCA Updates with CIS", (
            (43, 2, 0x206D, "LE Request Peer SCA"),
        ), errors)

    tx_iso = cis_central or cis_peripheral or iso_broadcaster
    rx_iso = cis_central or cis_peripheral or sync_receiver
    any_iso = tx_iso or sync_receiver

    if tx_iso:
        require_group(commands, "C.45 ISO transmit", (
            (41, 6, 0x2061, "LE Read ISO TX Sync"),
            (43, 5, 0x2070, "LE ISO Transmit Test"),
        ), errors)

    if rx_iso:
        require_group(commands, "C.46 ISO receive", (
            (43, 6, 0x2071, "LE ISO Receive Test"),
            (43, 7, 0x2072, "LE ISO Read Test Counters"),
        ), errors)

    if any_iso:
        require_group(commands, "C.47 ISO data path/test", (
            (43, 3, 0x206E, "LE Setup ISO Data Path"),
            (43, 4, 0x206F, "LE Remove ISO Data Path"),
            (44, 0, 0x2073, "LE ISO Test End"),
        ), errors)

    if cis_central or cis_peripheral or subrating:
        require_group(commands, "C.49 Host-set feature bits", (
            (44, 1, 0x2074, "LE Set Host Feature"),
        ), errors)

    # Read Buffer Size v2 becomes mandatory when HCI ISO traffic can be
    # transmitted (CIS Central/Peripheral or Isochronous Broadcaster).
    if tx_iso:
        require_group(commands, "C.55 ISO HCI buffers", (
            (41, 5, 0x2060, "LE Read Buffer Size v2"),
        ), errors)

    if power_control:
        require_group(commands, "C.51 LE Power Control Request", (
            (44, 3, 0x2076, "LE Enhanced Read Transmit Power Level"),
            (44, 4, 0x2077, "LE Read Remote Transmit Power Level"),
            (44, 7, 0x207A, "LE Set Transmit Power Reporting Enable"),
        ), errors)

    if path_loss:
        require_group(commands, "C.52 LE Path Loss Monitoring", (
            (44, 5, 0x2078, "LE Set Path Loss Reporting Parameters"),
            (44, 6, 0x2079, "LE Set Path Loss Reporting Enable"),
        ), errors)

    if subrating:
        require_group(commands, "C.57 Connection Subrating", (
            (46, 0, 0x207D, "LE Set Default Subrate"),
            (46, 1, 0x207E, "LE Subrate Request"),
        ), errors)

    # Channel Classification has mandatory Link Layer behavior (C.58), but
    # Core 5.4 does not add a new dedicated command bit for that condition.
    # LE Set Host Channel Classification is governed separately by C.36 and is
    # already part of the broad mandatory command coverage.
    if channel_class:
        print("C.58 active: Channel Classification advertised")

    if adv_coding:
        require_group(commands, "C.66 Advertising Coding Selection", (
            (46, 2, 0x207F, "LE Set Extended Advertising Parameters v2"),
        ), errors)

    if pawr_adv:
        require_group(commands, "C.67 PAwR Advertiser", (
            (46, 5, 0x2082, "LE Set Periodic Advertising Subevent Data"),
            (47, 1, 0x2086, "LE Set Periodic Advertising Parameters v2"),
        ), errors)

    if pawr_scan:
        require_group(commands, "C.68 PAwR Scanner", (
            (46, 6, 0x2083, "LE Set Periodic Advertising Response Data"),
            (46, 7, 0x2084, "LE Set Periodic Sync Subevent"),
        ), errors)

    active = []
    for label, enabled in (
        ("C.6", conn_param),
        ("C.39", cis_central),
        ("C.40", cis_peripheral),
        ("C.41", iso_broadcaster),
        ("C.42", sync_receiver),
        ("C.44", sca and (cis_central or cis_peripheral)),
        ("C.45", tx_iso),
        ("C.46", rx_iso),
        ("C.47", any_iso),
        ("C.49", cis_central or cis_peripheral or subrating),
        ("C.51", power_control),
        ("C.52", path_loss),
        ("C.55", tx_iso),
        ("C.57", subrating),
        ("C.58", channel_class),
        ("C.66", adv_coding),
        ("C.67", pawr_adv),
        ("C.68", pawr_scan),
    ):
        if enabled:
            active.append(label)
    print("Active feature-driven Core 5.4 conditions:", ", ".join(active))


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
        validate_conditional_commands(commands, features, errors)

        # LE CIS Peripheral uses the shared Connection Accept Timeout
        # configuration parameter.  These commands are mandatory in that
        # profile and are supplied by the compatibility dispatcher because the
        # multirole nrfxlib HCI table does not expose them directly.
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
                    "[ok] Connection Accept Timeout read/write: 0x%04X" % timeout
                )

        # Core 5.4 makes LE Read Supported States mandatory.  The exact value
        # is a product claim: this multirole resource profile advertises all
        # legacy combinations 0..41, with reserved bits 42..63 clear.
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
        elif states != EXPECTED_LE_SUPPORTED_STATES:
            errors.append(
                "LE Read Supported States is %s, expected %s for this resource "
                "profile"
                % (states.hex(" "), EXPECTED_LE_SUPPORTED_STATES.hex(" "))
            )
        else:
            print("[ok] LE Read Supported States:", states.hex(" "))

        # A 5.4-facing controller must not advertise the Core 6.0 extended
        # feature-set HCI commands through the 5.4 64-octet bitmap.
        for bit, opcode in zip((2, 3, 4), CORE_6_ONLY_OPCODES):
            if command_bit(commands, 47, bit):
                errors.append(
                    "Core 6.0 opcode 0x%04X still advertised in octet 47 bit %d"
                    % (opcode, bit)
                )

        # Direct attempts must not fall through to the newer SDC backend.
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

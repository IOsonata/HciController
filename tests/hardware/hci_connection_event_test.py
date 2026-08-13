#!/usr/bin/env python3
"""Regression for LE connection-complete event parsing."""

import sys

# hci_events, not hci_ble_test: this replays captured event bodies and has no
# board to talk to, so it must not pull in a module that exits when pyserial
# is missing.
from hci_events import parse_connection


def check(body_hex, expected):
    body = bytes.fromhex(body_hex)
    got = parse_connection(body)
    if got != expected:
        raise AssertionError("parse_connection returned %r, expected %r" %
                             (got, expected))


def main():
    # Exact LE Enhanced Connection Complete v2 events captured from the
    # two-controller CIS hardware run that exposed the missing 0x29 parser.
    check(
        "29 00 20 00 00 01 2b 8f fa 12 38 d7 "
        "00 00 00 00 00 00 00 00 00 00 00 00 "
        "28 00 00 00 90 01 00 ff ff ff",
        (0, 0x0020, 0, bytes.fromhex("2b 8f fa 12 38 d7"),
         0x0028, 0, 0x0190),
    )
    check(
        "29 00 24 00 01 01 ab d2 91 42 8b de "
        "00 00 00 00 00 00 00 00 00 00 00 00 "
        "28 00 00 00 90 01 07 ff ff ff",
        (0, 0x0024, 1, bytes.fromhex("ab d2 91 42 8b de"),
         0x0028, 0, 0x0190),
    )

    # The parser is shared by advertise/connect/probe; malformed meta events
    # must be ignored rather than throwing struct.error in those loops.
    if parse_connection(b"") is not None:
        raise AssertionError("empty event was accepted")
    if parse_connection(bytes.fromhex("01 00")) is not None:
        raise AssertionError("short Connection Complete was accepted")
    if parse_connection(bytes.fromhex("29 00 20 00")) is not None:
        raise AssertionError("short Enhanced Connection Complete v2 was accepted")

    print("[ok] LE connection complete parser accepts Enhanced v2 (0x29)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

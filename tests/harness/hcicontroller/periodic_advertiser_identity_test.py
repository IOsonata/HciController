#!/usr/bin/env python3
"""Regression for periodic-advertiser identity selection with multiple benches."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


EXPECTED_ADDR = bytes.fromhex("010203040506")
FOREIGN_ADDR = bytes.fromhex("111213141516")
ADDR_TYPE = 0x01
SID = pf.ADV_SID_PERIODIC


class FakeScanner:
    def __init__(self, packets):
        self.packets = list(packets)
        self.commands = []

    def identity(self):
        return bytes.fromhex("212223242526"), 0x01, "test"

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        return 0, b""

    def read_packet(self, timeout=1.0):
        if self.packets:
            return self.packets.pop(0)
        return None


class FakeAdvertiser:
    def read_packet(self, timeout=1.0):
        return None


def extended_report(addr, addr_type=ADDR_TYPE, sid=SID):
    data = pf.EXT_ADV_DISCOVERY_DATA
    report = bytearray(24 + len(data))
    report[2] = addr_type
    report[3:9] = addr
    report[11] = sid
    report[14:16] = struct.pack("<H", 0x00FF)
    report[23] = len(data)
    report[24:] = data
    body = bytes([pf.LE_EXTENDED_ADVERTISING_REPORT, 1]) + bytes(report)
    return pf.H4_EVENT, pf.EVT_LE_META, body


def assert_expected(result):
    # The discovery helper's interface is four fields. A previous change
    # accidentally returned the five-field raw report and broke both callers.
    assert len(result) == 4
    assert result[0] == ADDR_TYPE
    assert result[1] == EXPECTED_ADDR
    assert result[2] == SID
    assert result[3] == 0x00FF


def assert_create_sync(scanner):
    opcode, payload, _ = scanner.commands[-1]
    assert opcode == pf.OP_LE_PERIODIC_CREATE_SYNC
    assert payload[0] == 0
    assert payload[1] == SID
    assert payload[2] == ADDR_TYPE
    assert payload[3:9] == EXPECTED_ADDR


def main():
    # A different bench advertises the same marker/SID first. Ordinary
    # periodic-sync discovery must ignore it and wait for the intended AdvA.
    scanner = FakeScanner([
        extended_report(FOREIGN_ADDR),
        extended_report(EXPECTED_ADDR),
    ])
    result = pf._observe_periodic_advertiser(
        scanner,
        EXPECTED_ADDR,
        ADDR_TYPE,
        SID,
        timeout=0.1,
    )
    assert_expected(result)

    # Exercise the real caller too. This catches return-shape changes rather
    # than testing only the filter predicate.
    scanner = FakeScanner([
        extended_report(FOREIGN_ADDR),
        extended_report(EXPECTED_ADDR),
    ])
    pf._create_periodic_sync(scanner, EXPECTED_ADDR, ADDR_TYPE, SID)
    assert_create_sync(scanner)

    # PAwR uses the same discovery marker while servicing advertiser-side
    # subevent requests. It must apply the same exact identity filter.
    scanner = FakeScanner([
        extended_report(FOREIGN_ADDR),
        extended_report(EXPECTED_ADDR),
    ])
    result = pf._observe_periodic_advertiser_pawr(
        scanner,
        FakeAdvertiser(),
        [],
        EXPECTED_ADDR,
        ADDR_TYPE,
        SID,
        timeout=0.1,
    )
    assert_expected(result)

    # Exercise the PAwR caller that failed on hardware with a five-field tuple.
    scanner = FakeScanner([
        extended_report(FOREIGN_ADDR),
        extended_report(EXPECTED_ADDR),
    ])
    pf._create_periodic_sync_pawr(
        scanner,
        FakeAdvertiser(),
        [],
        EXPECTED_ADDR,
        ADDR_TYPE,
        SID,
    )
    assert_create_sync(scanner)

    # If only the other bench is visible, discovery must fail instead of
    # silently synchronizing to it.
    scanner = FakeScanner([extended_report(FOREIGN_ADDR)])
    try:
        pf._observe_periodic_advertiser(
            scanner,
            EXPECTED_ADDR,
            ADDR_TYPE,
            SID,
            timeout=0.01,
        )
    except pf.HciError:
        pass
    else:
        raise AssertionError("foreign periodic advertiser was accepted")

    print("periodic_advertiser_identity_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

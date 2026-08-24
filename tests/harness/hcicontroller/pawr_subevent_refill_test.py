#!/usr/bin/env python3
"""Regression for PAwR advertiser subevent-data refill."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


SYNC_HANDLE = 0x0123
SUBEVENT = 1
EMPTY_COUNTER = 32
DATA_COUNTER = 33


def periodic_v2(handle, event_counter, data):
    body = bytearray(11 + len(data))
    body[0] = pf.LE_PERIODIC_ADV_REPORT_V2
    body[1:3] = struct.pack("<H", handle)
    body[6:8] = struct.pack("<H", event_counter)
    body[8] = SUBEVENT
    body[10] = len(data)
    body[11:] = data
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


def data_request(start=SUBEVENT, count=1):
    body = bytes([
        pf.LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST,
        pf.ADV_HANDLE_PERIODIC,
        start,
        count,
    ])
    return pf.H4_EVENT, pf.EVT_LE_META, body


class FakeScanner:
    def __init__(self):
        self.pending = []
        self.fresh = [
            periodic_v2(SYNC_HANDLE, EMPTY_COUNTER, b""),
            periodic_v2(
                SYNC_HANDLE,
                DATA_COUNTER,
                pf.PAWR_SUBEVENT_MARKER,
            ),
        ]

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        if self.fresh:
            return self.fresh.pop(0)
        return None


class FakeAdvertiser:
    def __init__(self):
        self.pending = [data_request()]
        self.commands = []

    def has_pending_input(self):
        return False

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        return None

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        return 0, b""


def main():
    scanner = FakeScanner()
    advertiser = FakeAdvertiser()
    advertiser_deferred = []

    report = pf._wait_pawr_periodic_report(
        scanner,
        SYNC_HANDLE,
        pf.PAWR_SUBEVENT_MARKER,
        timeout=0.01,
        advertiser=advertiser,
        advertiser_deferred=advertiser_deferred,
    )

    assert report[0] == SYNC_HANDLE
    assert report[1] == DATA_COUNTER
    assert report[2] == SUBEVENT
    assert report[3] == pf.PAWR_SUBEVENT_MARKER

    assert advertiser_deferred == []
    assert advertiser.commands
    opcode, payload, allow_fail = advertiser.commands[-1]
    assert opcode == pf.OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA
    assert allow_fail is True
    assert payload[0] == pf.ADV_HANDLE_PERIODIC
    assert payload[1] == 1
    assert payload[2] == SUBEVENT
    assert pf.PAWR_SUBEVENT_MARKER in payload

    # The empty report was consumed as the trigger for the refill. It must not
    # be restored in front of the fresh marker report after the wait returns.
    assert scanner.pending == []

    print("pawr_subevent_refill_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

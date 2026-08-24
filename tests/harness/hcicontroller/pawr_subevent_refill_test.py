#!/usr/bin/env python3
"""Regression for PAwR advertiser subevent-data servicing order."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


SYNC_HANDLE = 0x0123
SUBEVENT = 1
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


class FakeAdvertiser:
    def __init__(self, order):
        self.pending = [data_request()]
        self.commands = []
        self.order = order

    def has_pending_input(self):
        return False

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        return None

    def command(self, opcode, payload=b"", allow_fail=False):
        self.order.append("refill")
        self.commands.append((opcode, payload, allow_fail))
        return 0, b""


class FakeScanner:
    def __init__(self, advertiser, order):
        self.pending = []
        self.advertiser = advertiser
        self.order = order
        self.fresh = [periodic_v2(
            SYNC_HANDLE,
            DATA_COUNTER,
            pf.PAWR_SUBEVENT_MARKER,
        )]
        self.late_request = data_request(start=0, count=1)

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        if not self.fresh:
            return None

        # This request becomes pending at the same instant the usable scanner
        # report is delivered. The Host must not service it before 0x2083.
        self.advertiser.pending.append(self.late_request)
        self.order.append("marker-report")
        return self.fresh.pop(0)


def main():
    order = []
    advertiser = FakeAdvertiser(order)
    scanner = FakeScanner(advertiser, order)
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

    # The first 0x27 was serviced before reading the marker-bearing 0x25.
    assert order == ["refill", "marker-report"]
    assert len(advertiser.commands) == 1
    opcode, payload, allow_fail = advertiser.commands[0]
    assert opcode == pf.OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA
    assert allow_fail is True
    assert payload[0] == pf.ADV_HANDLE_PERIODIC
    assert payload[1] == 1
    assert payload[2] == SUBEVENT
    assert pf.PAWR_SUBEVENT_MARKER in payload

    # The second 0x27 arrived with the usable 0x25 and remains pending. Once
    # the marker report is returned, no advertiser access is allowed before
    # the caller sends 0x2083.
    assert advertiser.pending == [scanner.late_request]
    assert advertiser_deferred == []
    assert scanner.pending == []

    print("pawr_subevent_refill_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

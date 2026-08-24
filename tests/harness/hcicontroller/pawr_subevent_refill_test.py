#!/usr/bin/env python3
"""Regression for independent PAwR advertiser subevent servicing."""

import threading
import time

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


START_SUBEVENT = 1
REQUEST_COUNT = 2


def data_request(start=START_SUBEVENT, count=REQUEST_COUNT):
    body = bytes([
        pf.LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST,
        pf.ADV_HANDLE_PERIODIC,
        start,
        count,
    ])
    return pf.H4_EVENT, pf.EVT_LE_META, body


class FakeAdvertiser:
    def __init__(self):
        self.pending = [
            data_request(),
            (pf.H4_EVENT, 0xFF, b"unrelated"),
        ]
        self.commands = []
        self.command_seen = threading.Event()

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        time.sleep(min(timeout, 0.001))
        return None

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        self.command_seen.set()
        return 0, b""


def main():
    hci = FakeAdvertiser()
    service = pf._start_pawr_data_service(hci)

    assert hci.command_seen.wait(1.0)
    pf._finish_pawr_data_service(hci, service)

    assert len(hci.commands) == 1
    opcode, payload, allow_fail = hci.commands[0]
    assert opcode == pf.OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA
    assert allow_fail is True

    assert payload[0] == pf.ADV_HANDLE_PERIODIC
    assert payload[1] == REQUEST_COUNT

    marker = pf.PAWR_SUBEVENT_MARKER
    element_len = 4 + len(marker)
    first = payload[2:2 + element_len]
    second = payload[2 + element_len:2 + 2 * element_len]

    assert first[0] == START_SUBEVENT
    assert first[1:3] == bytes([0, 1])
    assert first[3] == len(marker)
    assert first[4:] == marker

    # The requested sequence wraps from subevent 1 back to subevent 0.
    assert second[0] == 0
    assert second[1:3] == bytes([0, 1])
    assert second[3] == len(marker)
    assert second[4:] == marker

    # Non-0x27 advertiser events are restored after the worker is stopped.
    assert hci.pending == [(pf.H4_EVENT, 0xFF, b"unrelated")]
    assert service["finished"] is True
    assert service["errors"] == []

    print("pawr_subevent_refill_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

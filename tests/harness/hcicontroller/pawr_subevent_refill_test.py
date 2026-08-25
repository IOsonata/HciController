#!/usr/bin/env python3
"""-------------------------------------------------------------------------
@file	pawr_subevent_refill_test.py

@brief	Regression for independent PAwR subevent and response servicing.

		Verifies advertiser-side subevent refill, wrapped subevent numbering,
		response-report observation, and deferred event restoration while the
		PAwR data-service reader remains active.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------"""

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


def response_report(marker=pf.PAWR_RESPONSE_MARKER):
    # LE Periodic Advertising Response Report:
    # subevent, advertising handle, subevent, tx status, num responses,
    # then one response entry. The parser only needs data_len at entry + 5.
    body = bytearray(5 + 6 + len(marker))
    body[0] = pf.LE_PERIODIC_ADV_RESPONSE_REPORT
    body[1] = pf.ADV_HANDLE_PERIODIC
    body[2] = START_SUBEVENT
    body[3] = 0
    body[4] = 1
    body[5 + 5] = len(marker)
    body[5 + 6:] = marker
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


class FakeAdvertiser:
    def __init__(self):
        self.pending = [
            data_request(),
            response_report(),
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
    # A response may be reported on a later PAwR event. Do not retry until the
    # already-armed advertiser receiver has observed at least two full periodic
    # cycles of the configured train.
    assert pf.PAWR_RESPONSE_WAIT_CYCLES >= 2
    assert pf.PAWR_RESPONSE_WAIT >= 2 * pf.PAWR_INTERVAL_SECONDS

    hci = FakeAdvertiser()
    service = pf._start_pawr_data_service(hci)

    assert hci.command_seen.wait(1.0)
    assert service["response"].wait(1.0)
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

    # The same advertiser-side reader remains armed for the response report;
    # the marker-bearing 0x28 is consumed and signaled, not handed off to a
    # second reader after 0x2083.
    assert service["response"].is_set()

    # Other advertiser events are restored only after the worker is stopped.
    assert hci.pending == [(pf.H4_EVENT, 0xFF, b"unrelated")]
    assert service["finished"] is True
    assert service["errors"] == []

    print("pawr_subevent_refill_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""-------------------------------------------------------------------------
@file	pawr_sync_boundary_test.py

@brief	Regression for periodic-sync and PAwR command/event ordering.

		Verifies Create Sync generation boundaries, exact advertiser matching,
		and rejection of periodic reports received before the completed 0x2084
		subevent-selection command.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------"""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


ADV_ADDR = bytes.fromhex("010203040506")
OTHER_ADDR = bytes.fromhex("a1a2a3a4a5a6")
ADV_TYPE = 0x01
ADV_SID = pf.ADV_SID_PERIODIC
SYNC_HANDLE = 0x0123
OTHER_SYNC_HANDLE = 0x0234
SUBEVENT = 1
STALE_COUNTER = 7
WRONG_COUNTER = 8
FRESH_COUNTER = 9


def sync_established(
        handle, addr=ADV_ADDR, addr_type=ADV_TYPE, sid=ADV_SID,
        subevent=pf.LE_PERIODIC_SYNC_ESTABLISHED_V2, status=0):
    body = bytearray(20)
    body[0] = subevent
    body[1] = status
    body[2:4] = struct.pack("<H", handle)
    body[4] = sid
    body[5] = addr_type
    body[6:12] = addr
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


def periodic_v2(
        handle, event_counter, subevent=SUBEVENT,
        marker=pf.PAWR_SUBEVENT_MARKER):
    body = bytearray(11 + len(marker))
    body[0] = pf.LE_PERIODIC_ADV_REPORT_V2
    body[1:3] = struct.pack("<H", handle)
    body[6:8] = struct.pack("<H", event_counter)
    body[8] = subevent
    body[10] = len(marker)
    body[11:] = marker
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


def periodic_v1(handle, marker=b"old periodic report"):
    body = bytearray(8 + len(marker))
    body[0] = pf.LE_PERIODIC_ADV_REPORT
    body[1:3] = struct.pack("<H", handle)
    body[7] = len(marker)
    body[8:] = marker
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


class FakeCreateSyncHci:
    def __init__(self):
        self.unrelated = (pf.H4_EVENT, 0xFF, b"unrelated")
        self.pending = [
            sync_established(OTHER_SYNC_HANDLE, addr=OTHER_ADDR),
            self.unrelated,
        ]
        self.fresh = [
            sync_established(OTHER_SYNC_HANDLE + 1, addr=OTHER_ADDR),
            sync_established(SYNC_HANDLE),
        ]
        self.commands = []

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        if opcode == pf.OP_LE_PERIODIC_CREATE_SYNC:
            # Hci.command() can restore asynchronous events that it consumed
            # before the Create Sync Command Status. Such a Sync Established
            # event predates this procedure and must not satisfy its waiter.
            self.pending.insert(
                0,
                sync_established(OTHER_SYNC_HANDLE + 2, addr=OTHER_ADDR),
            )
        return 0, b""

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        if self.fresh:
            return self.fresh.pop(0)
        return None


class FakePawrHci:
    def __init__(self):
        self.pending = []
        self.fresh = [
            periodic_v2(SYNC_HANDLE, WRONG_COUNTER, subevent=0),
            periodic_v2(SYNC_HANDLE, FRESH_COUNTER, subevent=SUBEVENT),
        ]
        self.commands = []
        self.unrelated = (pf.H4_EVENT, 0xFF, b"unrelated")
        self.other_sync = periodic_v2(OTHER_SYNC_HANDLE, STALE_COUNTER)

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        if opcode == pf.OP_LE_SET_PERIODIC_SYNC_SUBEVENT:
            # These reports are observed by Hci.command() before the matching
            # 0x2084 Command Complete and restored to pending when it returns.
            # They are on the pre-completion side of the synchronization
            # boundary and must not supply Request_Event for 0x2083.
            self.pending.append(
                periodic_v2(SYNC_HANDLE, STALE_COUNTER, subevent=0)
            )
            self.pending.append(periodic_v1(SYNC_HANDLE))
            self.pending.append(self.other_sync)
            self.pending.append(self.unrelated)
        return 0, b""

    def read_packet(self, timeout=1.0):
        if self.pending:
            return self.pending.pop(0)
        if self.fresh:
            return self.fresh.pop(0)
        return None


def test_create_sync_boundary():
    hci = FakeCreateSyncHci()
    original_observe = pf._observe_periodic_advertiser
    pf._observe_periodic_advertiser = lambda *args, **kwargs: (
        ADV_TYPE, ADV_ADDR, ADV_SID, 0x00FF
    )
    try:
        source = pf._create_periodic_sync(hci, ADV_ADDR, ADV_TYPE, ADV_SID)
    finally:
        pf._observe_periodic_advertiser = original_observe

    assert source == (ADV_TYPE, ADV_ADDR, ADV_SID)
    opcode, payload, allow_fail = hci.commands[-1]
    assert opcode == pf.OP_LE_PERIODIC_CREATE_SYNC
    assert payload == (
        bytes([0, ADV_SID, ADV_TYPE])
        + ADV_ADDR
        + struct.pack("<HHB", 0, 0x0200, 0)
    )
    assert allow_fail is True

    # Every Sync Established event that was pending when Create Sync returned
    # is older than its Command Status. Only unrelated packets survive.
    assert hci.pending == [hci.unrelated]

    handle, body = pf._wait_sync_established(
        hci,
        expected_source=source,
        timeout=0.01,
        require_v2=True,
        keep_scan=True,
    )
    assert handle == SYNC_HANDLE
    assert pf._sync_established_source(body) == source

    # A later Sync Established event for another advertiser is consumed rather
    # than being restored to poison the next synchronization procedure.
    assert hci.fresh == []
    assert hci.pending == [hci.unrelated]


def test_pawr_subevent_boundary():
    hci = FakePawrHci()

    pf._set_pawr_sync_subevent(hci, SYNC_HANDLE, SUBEVENT)

    opcode, payload, allow_fail = hci.commands[-1]
    assert opcode == pf.OP_LE_SET_PERIODIC_SYNC_SUBEVENT
    assert payload == struct.pack("<HHB", SYNC_HANDLE, 0, 1) + bytes([SUBEVENT])
    assert allow_fail is True

    # All same-sync periodic reports deferred before the completed 0x2084 are
    # stale for the response procedure. Other syncs and unrelated packets are
    # preserved for their normal consumers.
    assert hci.pending == [hci.other_sync, hci.unrelated]

    report = pf._wait_pawr_periodic_report(
        hci,
        SYNC_HANDLE,
        SUBEVENT,
        pf.PAWR_SUBEVENT_MARKER,
        timeout=0.01,
    )
    assert report[0] == SYNC_HANDLE
    assert report[1] == FRESH_COUNTER
    assert report[2] == SUBEVENT
    assert report[3] == pf.PAWR_SUBEVENT_MARKER

    # The fresh wrong-subevent report was ignored rather than used for 0x2083.
    # Packets unrelated to this sync remain available.
    assert hci.fresh == []
    assert hci.pending == [hci.other_sync, hci.unrelated]


def main():
    test_create_sync_boundary()
    test_pawr_subevent_boundary()
    print("pawr_sync_boundary_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

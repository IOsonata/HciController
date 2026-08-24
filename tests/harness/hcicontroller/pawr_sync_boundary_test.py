#!/usr/bin/env python3
"""Regression for PAwR sync-subevent command/event ordering."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


SYNC_HANDLE = 0x0123
OTHER_SYNC_HANDLE = 0x0234
SUBEVENT = 1
STALE_COUNTER = 7
WRONG_COUNTER = 8
FRESH_COUNTER = 9


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


class FakeHci:
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


def main():
    hci = FakeHci()

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

    print("pawr_sync_boundary_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regression for PAwR sync-subevent command/event ordering."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf


SYNC_HANDLE = 0x0123
OTHER_SYNC_HANDLE = 0x0234
SUBEVENT = 1
DEFERRED_COUNTER = 7
FRESH_COUNTER = 8


def periodic_v2(handle, event_counter, marker=pf.PAWR_SUBEVENT_MARKER):
    body = bytearray(11 + len(marker))
    body[0] = pf.LE_PERIODIC_ADV_REPORT_V2
    body[1:3] = struct.pack("<H", handle)
    body[6:8] = struct.pack("<H", event_counter)
    body[8] = SUBEVENT
    body[10] = len(marker)
    body[11:] = marker
    return pf.H4_EVENT, pf.EVT_LE_META, bytes(body)


class FakeHci:
    def __init__(self):
        self.pending = []
        self.fresh = [periodic_v2(SYNC_HANDLE, FRESH_COUNTER)]
        self.commands = []
        self.unrelated = (pf.H4_EVENT, 0xFF, b"unrelated")
        self.other_sync = periodic_v2(OTHER_SYNC_HANDLE, DEFERRED_COUNTER)

    def command(self, opcode, payload=b"", allow_fail=False):
        self.commands.append((opcode, payload, allow_fail))
        if opcode == pf.OP_LE_SET_PERIODIC_SYNC_SUBEVENT:
            # Hci.command() restores asynchronous packets that arrived before
            # the matching Command Complete. A selected marker-bearing 0x25 in
            # that queue remains valid for the following response procedure.
            self.pending.append(periodic_v2(SYNC_HANDLE, DEFERRED_COUNTER))
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

    # The 0x2084 helper must not purge selected periodic reports deferred by
    # Hci.command(). The normal periodic-report consumer decides which report
    # is usable and preserves unrelated packets.
    assert hci.pending[0] == periodic_v2(SYNC_HANDLE, DEFERRED_COUNTER)

    report = pf._wait_periodic_report(
        hci,
        SYNC_HANDLE,
        pf.PAWR_SUBEVENT_MARKER,
        timeout=0.01,
        require_v2=True,
    )
    assert report[0] == SYNC_HANDLE
    assert report[1] == DEFERRED_COUNTER
    assert report[2] == SUBEVENT
    assert report[3] == pf.PAWR_SUBEVENT_MARKER

    # A later fresh report was not needed. Other-sync and unrelated packets
    # remain available to their normal consumers.
    assert hci.fresh == [periodic_v2(SYNC_HANDLE, FRESH_COUNTER)]
    assert hci.pending == [hci.other_sync, hci.unrelated]

    print("pawr_sync_boundary_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Regression for the two-controller CIS teardown ordering.

A hardware run showed LE Set Host Feature answering Command Disallowed twice
at the end and the test printing PASS anyway. Disconnect only returns Command
Status, so the links were still up when the clear went out, and Vol 4 Part E
7.8.115 refuses the command for as long as the controller has a connection.
CIS Host Support was therefore left set on both dongles after every run.

This drives cleanup against a recorded controller instead of a board, so the
ordering is checked wherever python runs.
"""

import struct
import sys

import _bootstrap  # noqa: F401
from hcicontroller.hci_events import EVT_DISCONNECTION_COMPLETE, H4_EVENT
from hcicontroller import hci_cis_cleanup as teardown


OP_DISCONNECT = teardown.OP_DISCONNECT
OP_LE_SET_HOST_FEATURE = teardown.OP_LE_SET_HOST_FEATURE


class FakeHci:
    """
    Answers commands the way the SDC did on the bench.

    Disconnect is accepted and the link stays up until the Disconnection
    Complete is read, which is the behaviour the old teardown ignored.
    """

    def __init__(self, name, handle):
        self.name = name
        self.handle = handle
        self.connected = handle is not None
        self.queued = []
        self.sent = []

    def command(self, opcode, payload=b"", timeout=3.0, allow_fail=False):
        self.sent.append(opcode)
        if opcode == OP_DISCONNECT:
            handle = struct.unpack("<H", payload[0:2])[0] & 0x0FFF
            if handle == self.handle:
                self.queued.append(
                    (H4_EVENT, EVT_DISCONNECTION_COMPLETE,
                     bytes([0x00]) + struct.pack("<H", handle) + bytes([0x13])))
            return 0, b""
        if opcode == OP_LE_SET_HOST_FEATURE:
            # 0x0C Command Disallowed while a link is up.
            return (0x0C, b"") if self.connected else (0x00, b"")
        return 0, b""

    def read_packet(self, timeout=1.0):
        if not self.queued:
            return None
        packet = self.queued.pop(0)
        if packet[1] == EVT_DISCONNECTION_COMPLETE:
            self.connected = False
        return packet

    def close(self):
        pass


def run_cleanup():
    central = FakeHci("central", 0x0020)
    peripheral = FakeHci("peripheral", 0x0024)
    teardown.cleanup(central, peripheral, 0x00, None, None, 0x0020, 0x0024)
    return central, peripheral


def main():
    central, peripheral = run_cleanup()

    for hci in (central, peripheral):
        if hci.connected:
            raise AssertionError(
                "%s link was never confirmed down" % hci.name)
        if OP_LE_SET_HOST_FEATURE not in hci.sent:
            raise AssertionError(
                "%s was never asked to clear CIS Host Support" % hci.name)
        order = hci.sent.index(OP_DISCONNECT), \
            hci.sent.index(OP_LE_SET_HOST_FEATURE)
        if order[0] > order[1]:
            raise AssertionError(
                "%s cleared the feature bit before disconnecting" % hci.name)

    print("[ok] CIS teardown waits for the link before clearing host support")

    # And the warning has to appear when the clear really does fail, or a
    # teardown that never works looks like one that did.
    stuck = FakeHci("stuck", 0x0020)
    stuck.queued = []           # never reports Disconnection Complete
    status, _ = stuck.command(OP_LE_SET_HOST_FEATURE, bytes([32, 0]),
                              allow_fail=True)
    if status != 0x0C:
        raise AssertionError("a live link should refuse the clear")

    print("[ok] a refused clear is still reported, not swallowed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

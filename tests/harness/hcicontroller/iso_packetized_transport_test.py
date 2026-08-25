#!/usr/bin/env python3
"""Regression for IsoHci receive through the packetized transport layer."""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import hci_iso as iso


class FakePacketTransport:
    def __init__(self, packet):
        self.packet = packet
        self.reads = 0

    def read_packet(self, timeout=1.0):
        del timeout
        self.reads += 1
        packet = self.packet
        self.packet = None
        return packet



def main():
    handle = 0x0123
    sequence = 7
    sdu = b"ISO packetized transport"
    handle_flags = (handle & 0x0FFF) | (iso.ISO_PB_COMPLETE << 12)
    load = struct.pack("<HH", sequence, len(sdu)) + sdu
    body = struct.pack("<HH", handle_flags, len(load)) + load
    packet = (iso.H4_ISO, None, body)

    hci = iso.IsoHci.__new__(iso.IsoHci)
    hci.raw = False
    hci.transport = FakePacketTransport(packet)

    # IsoHci must inherit the common Hci.read_wire() packetized receive path.
    # No serial object and no byte-stream read_exact() are present here.
    assert hci.read_wire(0.1) == packet
    assert hci.transport.reads == 1

    parsed = iso.parse_iso(body)
    assert parsed["handle"] == handle
    assert parsed["sequence"] == sequence
    assert parsed["sdu_len"] == len(sdu)
    assert parsed["fragment"] == sdu

    print("iso_packetized_transport_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

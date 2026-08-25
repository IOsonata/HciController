#!/usr/bin/env python3
"""-------------------------------------------------------------------------
@file	iso_packetized_transport_test.py

@brief	Regression for ISO receive through the packetized transport layer.

		Verifies that both ISO HCI helpers use the common packetized receive
		path and that ISO packet parsing preserves handle, sequence, length,
		and payload data.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------"""

import struct

import _bootstrap  # noqa: F401
from hcicontroller import hci_iso as iso
from hcicontroller import hci_cis_pair_test as cis


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


def exercise_iso_hci(cls, packet):
    # ISO helpers must inherit the common Hci.read_wire() packetized receive
    # path. No serial object and no byte-stream read_exact() are present here.
    assert "read_wire" not in cls.__dict__
    hci = cls.__new__(cls)
    hci.raw = False
    hci.transport = FakePacketTransport(packet)
    assert hci.read_wire(0.1) == packet
    assert hci.transport.reads == 1


def main():
    handle = 0x0123
    sequence = 7
    sdu = b"ISO packetized transport"
    handle_flags = (handle & 0x0FFF) | (iso.ISO_PB_COMPLETE << 12)
    load = struct.pack("<HH", sequence, len(sdu)) + sdu
    body = struct.pack("<HH", handle_flags, len(load)) + load
    packet = (iso.H4_ISO, None, body)

    # The shared ISO helper and the standalone CIS helper are both used by the
    # release suite. Keep both on the transport-owned packet parser.
    exercise_iso_hci(iso.IsoHci, packet)
    exercise_iso_hci(cis.IsoHci, packet)

    parsed = iso.parse_iso(body)
    assert parsed["handle"] == handle
    assert parsed["sequence"] == sequence
    assert parsed["sdu_len"] == len(sdu)
    assert parsed["fragment"] == sdu

    print("iso_packetized_transport_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""-------------------------------------------------------------------------
@file	serial_h4_transport_test.py

@brief	Regression tests for nonblocking serial H:4 packet reassembly.

		Verifies short polling, partial packet retention, multiple buffered
		packets, ACL and ISO fragmentation, delayed input, and invalid H:4
		indicator handling.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------"""

import threading
import time

import _bootstrap  # noqa: F401
from hcicontroller import hci_transport as ht


class FakeSerialException(Exception):
    pass


class FakeSerialModule:
    SerialException = FakeSerialException


class FakeSerial:
    def __init__(self):
        self._rx = bytearray()
        self.read_sizes = []
        self.timeout = 0.05

    @property
    def in_waiting(self):
        return len(self._rx)

    def read(self, size):
        self.read_sizes.append(size)
        if size <= 0:
            return b""
        if not self._rx:
            time.sleep(self.timeout)
            return b""
        count = min(size, len(self._rx))
        data = bytes(self._rx[:count])
        del self._rx[:count]
        return data

    def feed(self, data):
        self._rx.extend(data)


def fake_transport():
    transport = ht.SerialH4Transport.__new__(ht.SerialH4Transport)
    transport._serial_module = FakeSerialModule
    transport.ser = FakeSerial()
    transport._rx = bytearray()
    return transport


def main():
    # The real serial port has a 50 ms default timeout. A PAwR poll may ask for
    # only 2 ms, so an empty poll must not call the blocking serial read.
    transport = fake_transport()
    assert transport.read_packet(0.002) is None
    assert transport.ser.read_sizes == []

    # A zero-timeout poll must not consume the H:4 Event indicator and then
    # abandon the packet when its header arrives in a later poll.
    transport = fake_transport()
    transport.ser.feed(bytes([ht.H4_EVENT]))
    assert transport.read_packet(0.0) is None
    assert bytes(transport._rx) == bytes([ht.H4_EVENT])

    transport.ser.feed(bytes([0x3E]))
    assert transport.read_packet(0.0) is None
    assert bytes(transport._rx) == bytes([ht.H4_EVENT, 0x3E])

    transport.ser.feed(bytes([2, 0xAA, 0xBB]))
    assert transport.read_packet(0.0) == (
        ht.H4_EVENT, 0x3E, bytes([0xAA, 0xBB])
    )
    assert not transport._rx

    # Multiple packets may arrive in one serial chunk. Returning the first one
    # must retain the next complete packet for the following nonblocking poll.
    transport = fake_transport()
    transport.ser.feed(bytes([
        ht.H4_EVENT, 0x0E, 1, 0x11,
        ht.H4_EVENT, 0x0F, 2, 0x22, 0x33,
    ]))
    assert transport.read_packet(0.0) == (ht.H4_EVENT, 0x0E, bytes([0x11]))
    assert transport.read_packet(0.0) == (
        ht.H4_EVENT, 0x0F, bytes([0x22, 0x33])
    )
    assert transport.read_packet(0.0) is None

    # ACL and ISO headers can be split at arbitrary byte boundaries too.
    transport = fake_transport()
    acl = bytes([ht.H4_ACL, 0x01, 0x20, 3, 0]) + b"ACL"
    transport.ser.feed(acl[:3])
    assert transport.read_packet(0.0) is None
    transport.ser.feed(acl[3:6])
    assert transport.read_packet(0.0) is None
    transport.ser.feed(acl[6:])
    assert transport.read_packet(0.0) == (ht.H4_ACL, None, acl[1:])

    transport = fake_transport()
    iso = bytes([ht.H4_ISO, 0x02, 0x10, 3, 0]) + b"ISO"
    transport.ser.feed(iso[:4])
    assert transport.read_packet(0.0) is None
    transport.ser.feed(iso[4:])
    assert transport.read_packet(0.0) == (ht.H4_ISO, None, iso[1:])

    # A blocking call waits for later bytes while retaining the same partial
    # packet state used by zero-timeout polling.
    transport = fake_transport()

    def delayed_event():
        time.sleep(0.005)
        transport.ser.feed(bytes([ht.H4_EVENT, 0x3E, 1, 0x44]))

    thread = threading.Thread(target=delayed_event)
    thread.start()
    try:
        assert transport.read_packet(0.1) == (
            ht.H4_EVENT, 0x3E, bytes([0x44])
        )
    finally:
        thread.join()

    # An invalid packet indicator is still a hard stream-sync error.
    transport = fake_transport()
    transport.ser.feed(b"\x7f")
    try:
        transport.read_packet(0.0)
    except ht.TransportError as err:
        assert "bad H:4 packet indicator" in str(err)
    else:
        raise AssertionError("invalid H:4 indicator was accepted")

    print("serial_h4_transport_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

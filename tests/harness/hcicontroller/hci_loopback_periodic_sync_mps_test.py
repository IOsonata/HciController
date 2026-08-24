#!/usr/bin/env python3
"""Run periodic-sync PRBS stress with one-packet native USB Event-IN reads.

This wrapper changes only the host Event-IN read size.  The shared transport,
stress test, and firmware remain unchanged.  Native Bluetooth Event-IN reads
are capped to the endpoint wMaxPacketSize so each synchronous PyUSB/libusb read
covers at most one interrupt transaction.
"""

import sys

import _bootstrap  # noqa: F401
from hcicontroller import hci_transport as transport


_native_usb_read_endpoint = transport.NativeUsbTransport._read_endpoint


def _read_one_event_transaction(self, endpoint, size, timeout_ms):
    if endpoint is getattr(self, "event_ep", None):
        mps = int(getattr(endpoint, "wMaxPacketSize", 0)) & 0x07FF
        if mps > 0:
            size = min(int(size), mps)
    return _native_usb_read_endpoint(self, endpoint, size, timeout_ms)


# Install the cap before importing the existing stress test so every
# NativeUsbTransport it opens uses packet-sized Event-IN reads.
transport.NativeUsbTransport._read_endpoint = _read_one_event_transaction

import hci_loopback_periodic_sync_test as stress


if __name__ == "__main__":
    print("Native USB Event-IN stress: reads capped to endpoint MPS")
    sys.exit(stress.main())

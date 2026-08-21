#!/usr/bin/env python3
"""Run the periodic failure diagnostic with one-packet native USB Event-IN reads.

This wrapper changes only the diagnostic host read size.  The shared
hci_transport implementation and firmware remain unchanged.  Native Bluetooth
Event-IN reads are capped to the endpoint wMaxPacketSize so one synchronous
PyUSB/libusb request cannot span several interrupt transactions and lose an
already-received prefix when the request times out.
"""

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_HARNESS_DIR = _HERE.parent
_LIB_DIR = _HARNESS_DIR / "lib"

for path in (_HERE, _HARNESS_DIR, _LIB_DIR):
    text = str(path)
    if text not in sys.path:
        sys.path.insert(0, text)

import hci_transport as transport


_native_usb_read_endpoint = transport.NativeUsbTransport._read_endpoint


def _read_one_event_transaction(self, endpoint, size, timeout_ms):
    if endpoint is getattr(self, "event_ep", None):
        mps = int(getattr(endpoint, "wMaxPacketSize", 0)) & 0x07FF
        if mps > 0:
            size = min(int(size), mps)
    return _native_usb_read_endpoint(self, endpoint, size, timeout_ms)


# Install this before importing periodic_failure_diag_safe.  The safe
# diagnostic wraps NativeUsbTransport._read_endpoint to record raw host reads;
# therefore its trace sees the actual packet-sized reads produced here.
transport.NativeUsbTransport._read_endpoint = _read_one_event_transaction

import periodic_failure_diag_safe as safe


if __name__ == "__main__":
    print("Native USB Event-IN diagnostic: reads capped to endpoint MPS")
    sys.exit(safe.diag.main())

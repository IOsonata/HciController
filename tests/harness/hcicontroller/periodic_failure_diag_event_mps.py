#!/usr/bin/env python3
"""Run legacy native USB periodic diagnostics one Event-IN packet per host read."""

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

# Install the semantic event validation used by the existing focused capture.
import periodic_failure_diag_safe  # noqa: F401
import periodic_failure_diag as diag
from lib import hci_transport as transport


_original_read_endpoint = transport.NativeUsbTransport._read_endpoint


def _read_endpoint_one_event_packet(self, endpoint, size, timeout_ms):
    """Limit legacy Event IN reads to one USB max-packet transaction."""
    if (not self.bulk_serialization
            and self.event_ep is not None
            and endpoint is self.event_ep):
        mps = int(getattr(endpoint, "wMaxPacketSize", size))
        if mps > 0:
            size = min(size, mps)
    return _original_read_endpoint(self, endpoint, size, timeout_ms)


transport.NativeUsbTransport._read_endpoint = _read_endpoint_one_event_packet


if __name__ == "__main__":
    sys.exit(diag.main())

#!/usr/bin/env python3
"""Run the production PAwR release path with blocking serial waits.

This keeps the current buffered H:4 packet reassembly and caller-bounded
timeouts, but removes the 1 ms Python polling loop from SerialH4Transport.
"""

import time

import _bootstrap  # noqa: F401
from hcicontroller import hci_transport as ht
import release_test


def _read_packet_blocking(self, timeout=1.0):
    timeout = max(0.0, timeout)
    deadline = time.monotonic() + timeout

    while True:
        packet = self._take_packet()
        if packet is not None:
            return packet

        try:
            waiting = int(self.ser.in_waiting)
            if waiting > 0:
                chunk = self.ser.read(waiting)
            elif timeout <= 0:
                return None
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None

                # Preserve the pre-reorg blocking behavior without violating a
                # short caller timeout. Cap each driver wait at the historical
                # 50 ms serial timeout so worker shutdown stays responsive.
                old_timeout = self.ser.timeout
                self.ser.timeout = min(0.05, remaining)
                try:
                    chunk = self.ser.read(1)
                finally:
                    self.ser.timeout = old_timeout
        except (self._serial_module.SerialException, OSError) as err:
            raise ht.TransportGone(str(err))

        if chunk:
            self._rx.extend(chunk)
            continue

        if time.monotonic() >= deadline:
            return None


def main():
    ht.SerialH4Transport.read_packet = _read_packet_blocking
    return release_test.main()


if __name__ == "__main__":
    raise SystemExit(main())

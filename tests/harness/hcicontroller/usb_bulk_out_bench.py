#!/usr/bin/env python3
"""Raw native Bluetooth USB Bulk OUT throughput benchmark.

Build the firmware with HCI_USB_BENCHMARK=1. In that mode complete ACL packets
are consumed inside HciUsb before they reach HciController/SDC, so the measured
path is host Bulk OUT -> USB receive/parse -> immediate rearm. The benchmark is
shared by usb_hci_native and usb_hci_native_opt for direct A/B comparison.

By default writes use PyUSB's synchronous endpoint.write(). --async-depth uses
libusb asynchronous bulk transfers through PyUSB's libusb1 backend so several
host transfers can remain outstanding at once.
"""

import argparse
import statistics
import sys
import time

import _bootstrap  # noqa: F401
from hcicontroller import hci_transport


MAX_HCI_PACKET = 1024
ACL_HEADER_SIZE = 4


def parse_sizes(text):
    sizes = []
    for item in text.split(","):
        try:
            size = int(item.strip(), 0)
        except ValueError as err:
            raise argparse.ArgumentTypeError("invalid packet size %r" % item) from err
        if size < ACL_HEADER_SIZE or size > MAX_HCI_PACKET:
            raise argparse.ArgumentTypeError(
                "packet size must be %d..%d bytes" %
                (ACL_HEADER_SIZE, MAX_HCI_PACKET)
            )
        sizes.append(size)
    if not sizes:
        raise argparse.ArgumentTypeError("at least one packet size is required")
    return sizes


def acl_packet(size):
    payload_len = size - ACL_HEADER_SIZE
    packet = bytearray(size)
    packet[0] = 0x01
    packet[1] = 0x00
    packet[2] = payload_len & 0xFF
    packet[3] = (payload_len >> 8) & 0xFF
    for index in range(payload_len):
        packet[ACL_HEADER_SIZE + index] = index & 0xFF
    return packet


def write_exact(endpoint, packet, timeout_ms):
    written = endpoint.write(packet, timeout=timeout_ms)
    if written != len(packet):
        raise hci_transport.TransportError(
            "short Bulk OUT write: %d of %d bytes" % (written, len(packet))
        )


class AsyncBulkWriter:
    """Keep multiple libusb Bulk OUT transfers outstanding on one endpoint."""

    def __init__(self, transport, endpoint, packet, depth, timeout_ms):
        try:
            import ctypes
            import usb.backend.libusb1 as libusb1
        except ImportError as err:
            raise hci_transport.TransportError(
                "asynchronous mode requires PyUSB's libusb1 backend"
            ) from err

        device = transport.device
        backend = getattr(getattr(device, "_ctx", None), "backend", None)
        if backend is None or backend.__class__.__module__ != libusb1.__name__:
            raise hci_transport.TransportError(
                "asynchronous mode requires the PyUSB libusb1 backend"
            )

        device._ctx.managed_open()
        dev_handle = device._ctx.handle
        if dev_handle is None or not hasattr(dev_handle, "handle"):
            raise hci_transport.TransportError(
                "cannot access the active libusb device handle"
            )

        required = (
            "_libusb_transfer_cb_fn_p",
            "_libusb_transfer_p",
            "LIBUSB_TRANSFER_COMPLETED",
            "LIBUSB_TRANSFER_TIMED_OUT",
            "LIBUSB_TRANSFER_NO_DEVICE",
            "LIBUSB_ERROR_INTERRUPTED",
        )
        missing = [name for name in required if not hasattr(libusb1, name)]
        if missing:
            raise hci_transport.TransportError(
                "PyUSB libusb1 backend lacks async definitions: %s" %
                ", ".join(missing)
            )

        self._ctypes = ctypes
        self._libusb1 = libusb1
        self._lib = backend.lib
        self._ctx = backend.ctx
        self._dev_handle = dev_handle.handle
        self._endpoint = int(endpoint.bEndpointAddress)
        self._packet = bytes(packet)
        self._depth = int(depth)
        self._timeout_ms = int(timeout_ms)
        self._transfer_type_bulk = getattr(
            libusb1, "_LIBUSB_TRANSFER_TYPE_BULK", 2
        )

        for symbol in (
            "libusb_alloc_transfer",
            "libusb_free_transfer",
            "libusb_submit_transfer",
            "libusb_cancel_transfer",
            "libusb_handle_events",
        ):
            if not hasattr(self._lib, symbol):
                raise hci_transport.TransportError(
                    "libusb is missing %s" % symbol
                )

        self._lib.libusb_cancel_transfer.argtypes = [
            libusb1._libusb_transfer_p
        ]
        self._lib.libusb_cancel_transfer.restype = ctypes.c_int

        self._callback = libusb1._libusb_transfer_cb_fn_p(self._on_complete)
        self._slots = []
        self._slot_by_address = {}
        self._target = 0
        self._submitted = 0
        self._completed = 0
        self._active = 0
        self._error = None
        self._aborting = False

        buffer_type = ctypes.c_ubyte * len(self._packet)
        try:
            for _ in range(self._depth):
                transfer = self._lib.libusb_alloc_transfer(0)
                if not transfer:
                    raise hci_transport.TransportError(
                        "libusb_alloc_transfer failed"
                    )
                buffer = buffer_type.from_buffer_copy(self._packet)
                td = transfer.contents
                td.dev_handle = self._dev_handle
                td.endpoint = self._endpoint
                td.type = self._transfer_type_bulk
                td.timeout = self._timeout_ms
                td.buffer = ctypes.cast(buffer, ctypes.c_void_p)
                td.length = len(self._packet)
                td.callback = self._callback
                td.num_iso_packets = 0

                slot = {
                    "transfer": transfer,
                    "buffer": buffer,
                    "active": False,
                }
                self._slots.append(slot)
                self._slot_by_address[
                    ctypes.addressof(transfer.contents)
                ] = slot
        except Exception:
            self.close()
            raise

    def _libusb_error(self, code):
        strerror = getattr(self._libusb1, "_strerror", None)
        if strerror is not None:
            try:
                return "%s (%d)" % (strerror(code), code)
            except Exception:
                pass
        return "libusb error %d" % code

    def _set_error(self, message):
        if self._error is None:
            self._error = message
        self._aborting = True

    def _submit(self, slot):
        transfer = slot["transfer"]
        transfer.contents.actual_length = 0
        ret = self._lib.libusb_submit_transfer(transfer)
        if ret < 0:
            self._set_error("async Bulk OUT submit failed: %s" %
                            self._libusb_error(ret))
            return False

        slot["active"] = True
        self._active += 1
        self._submitted += 1
        return True

    def _on_complete(self, transfer):
        address = self._ctypes.addressof(transfer.contents)
        slot = self._slot_by_address.get(address)
        if slot is None:
            self._set_error("unknown libusb transfer completed")
            return

        if slot["active"]:
            slot["active"] = False
            self._active -= 1

        if self._aborting:
            return

        td = transfer.contents
        if td.status != self._libusb1.LIBUSB_TRANSFER_COMPLETED:
            self._set_error(
                "async Bulk OUT completion status=%d actual=%d expected=%d" %
                (td.status, td.actual_length, len(self._packet))
            )
            return
        if td.actual_length != len(self._packet):
            self._set_error(
                "short async Bulk OUT write: %d of %d bytes" %
                (td.actual_length, len(self._packet))
            )
            return

        self._completed += 1

        if self._submitted < self._target:
            self._submit(slot)

    def _handle_events(self):
        ret = self._lib.libusb_handle_events(self._ctx)
        if ret < 0 and ret != self._libusb1.LIBUSB_ERROR_INTERRUPTED:
            self._set_error("libusb_handle_events failed: %s" %
                            self._libusb_error(ret))

    def _cancel_active(self):
        self._aborting = True
        for slot in self._slots:
            if not slot["active"]:
                continue
            ret = self._lib.libusb_cancel_transfer(slot["transfer"])
            if ret < 0:
                continue

        while self._active > 0:
            ret = self._lib.libusb_handle_events(self._ctx)
            if ret < 0 and ret != self._libusb1.LIBUSB_ERROR_INTERRUPTED:
                break

    def run(self, count):
        self._target = int(count)
        self._submitted = 0
        self._completed = 0
        self._active = 0
        self._error = None
        self._aborting = False

        initial = min(self._depth, self._target)
        for index in range(initial):
            if not self._submit(self._slots[index]):
                break

        if self._error is not None:
            self._cancel_active()
            raise hci_transport.TransportError(self._error)

        start = time.perf_counter()
        try:
            while self._completed < self._target and self._error is None:
                self._handle_events()
        except KeyboardInterrupt:
            self._cancel_active()
            raise

        elapsed = time.perf_counter() - start

        if self._error is not None:
            self._cancel_active()
            raise hci_transport.TransportError(self._error)
        if self._active != 0:
            self._cancel_active()
            raise hci_transport.TransportError(
                "async Bulk OUT ended with %d transfer(s) still active" %
                self._active
            )
        if self._completed != self._target:
            raise hci_transport.TransportError(
                "async Bulk OUT completed %d of %d transfers" %
                (self._completed, self._target)
            )
        return elapsed

    def close(self):
        if self._active > 0:
            self._cancel_active()
        for slot in self._slots:
            transfer = slot.get("transfer")
            if transfer:
                self._lib.libusb_free_transfer(transfer)
                slot["transfer"] = None
        self._slots = []
        self._slot_by_address = {}


def run_size(transport, endpoint, size, count, repeats, warmup,
             timeout_ms, async_depth):
    packet = acl_packet(size)

    for _ in range(warmup):
        write_exact(endpoint, packet, timeout_ms)

    throughputs = []
    packet_rates = []
    elapsed_values = []

    for run in range(1, repeats + 1):
        if async_depth > 0:
            writer = AsyncBulkWriter(
                transport, endpoint, packet, async_depth, timeout_ms
            )
            try:
                elapsed = writer.run(count)
            finally:
                writer.close()
        else:
            start = time.perf_counter()
            for _ in range(count):
                write_exact(endpoint, packet, timeout_ms)
            elapsed = time.perf_counter() - start

        total_bytes = size * count
        mib_per_second = total_bytes / elapsed / (1024.0 * 1024.0)
        packets_per_second = count / elapsed
        elapsed_values.append(elapsed)
        throughputs.append(mib_per_second)
        packet_rates.append(packets_per_second)

        print(
            "size=%4d run=%d count=%d time=%8.3f s  %10.1f pkt/s  %7.3f MiB/s"
            % (size, run, count, elapsed, packets_per_second, mib_per_second)
        )

    print(
        "size=%4d median                 %10.1f pkt/s  %7.3f MiB/s"
        % (size, statistics.median(packet_rates), statistics.median(throughputs))
    )
    return statistics.median(elapsed_values), statistics.median(throughputs)


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark native HCI USB Bulk OUT without Bluetooth radio traffic"
    )
    parser.add_argument(
        "--usb",
        help="USB serial number or VID:PID; omit when exactly one native controller is attached",
    )
    parser.add_argument(
        "--sizes",
        type=parse_sizes,
        default=parse_sizes("64,256,512,1024"),
        help="comma-separated ACL packet sizes in bytes (default: 64,256,512,1024)",
    )
    parser.add_argument("--count", type=int, default=10000,
                        help="packets per timed run (default: 10000)")
    parser.add_argument("--repeat", type=int, default=3,
                        help="timed runs per packet size (default: 3)")
    parser.add_argument("--warmup", type=int, default=100,
                        help="untimed packets before each packet size (default: 100)")
    parser.add_argument("--timeout-ms", type=int, default=2000,
                        help="per-transfer USB timeout in milliseconds (default: 2000)")
    parser.add_argument(
        "--async-depth", type=int, default=0,
        help=("number of libusb Bulk OUT transfers kept outstanding; "
              "0 uses synchronous PyUSB writes (default: 0)"),
    )
    args = parser.parse_args()

    if (args.count <= 0 or args.repeat <= 0 or args.warmup < 0 or
            args.timeout_ms <= 0 or args.async_depth < 0):
        parser.error(
            "count/repeat/timeout must be positive; warmup/async-depth must be nonnegative"
        )

    spec = hci_transport.discover(kind="usb", usb_selector=args.usb)
    if spec is None:
        raise hci_transport.SelectionError("no native USB HCI controller found")

    print("controller: %s" % spec)
    print("firmware: HCI_USB_BENCHMARK=1")
    print("path: host Bulk OUT -> HciUsb sink -> immediate rearm")

    transport = spec.open()
    try:
        if transport.bulk_serialization:
            raise hci_transport.TransportError(
                "benchmark requires legacy native USB alternate setting 0"
            )

        endpoint = transport.bulk_out_ep
        print("Bulk OUT endpoint: 0x%02X" % endpoint.bEndpointAddress)
        if args.async_depth > 0:
            print("submission: libusb async depth=%d" % args.async_depth)
        else:
            print("submission: PyUSB synchronous")
        print("count=%d repeat=%d warmup=%d" %
              (args.count, args.repeat, args.warmup))

        for size in args.sizes:
            run_size(
                transport, endpoint, size, args.count, args.repeat,
                args.warmup, args.timeout_ms, args.async_depth
            )
    finally:
        transport.close()

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (hci_transport.TransportError,
            hci_transport.SelectionError,
            KeyboardInterrupt) as err:
        print("ERROR: %s" % err, file=sys.stderr)
        sys.exit(1)

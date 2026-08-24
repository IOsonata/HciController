#!/usr/bin/env python3
"""Run periodic_failure_diag with validation at both native USB Event boundaries."""

import struct
import sys
import time
import zlib
from collections import deque

import _bootstrap  # noqa: F401
import periodic_failure_diag as diag
from hcicontroller import hci_ble_test as hci_test
from hcicontroller import hci_transport as transport
from hcicontroller.hci_events import EVT_NUM_COMPLETED_PACKETS, H4_EVENT
from hcicontroller.hci_pair import HciError, HciGone


# Counter block v8 appends thirteen packed EP1 acknowledgement records. Keep
# the imported reader quiet while still letting older firmware return a shorter
# block; read_counters() already consumes only the words actually present.
hci_test.COUNTER_VERSION = 8
hci_test._impl.COUNTER_VERSION = 8

_USB_EVENT_COUNTER_NAMES = [
    "USB Event-IN host transactions",
    "USB Event-IN DMA completions",
    "USB Event-IN continuations",
    "USB Event-IN logical completions",
    "USB Event-IN bad AMOUNT",
    "USB Event-IN stale status on arm",
    "USB Event-IN/EPOUT2 collisions",
    "USB Event-IN late status",
    "USB Event-IN/EPOUT2 END overlap",
]
if len(diag.COUNTER_NAMES) == 40:
    diag.COUNTER_NAMES.extend(_USB_EVENT_COUNTER_NAMES)

_USB_EVENT_COUNTER_LIMIT = 49
_USB_EVENT_ACK_TRACE_FIRST = 49
_USB_EVENT_ACK_TRACE_DEPTH = 13
_USB_EVENT_ACK_TRACE_END = _USB_EVENT_ACK_TRACE_FIRST + _USB_EVENT_ACK_TRACE_DEPTH
_HOST_EVENT_READ_TRACE_DEPTH = 96

_USB_TX_VALIDATION_OPCODE = 0xFFF2
_USB_TX_VALIDATION_VERSION = 1
_USB_TX_VALIDATION_RECORD_LEN = 28
_USB_TX_VALIDATION_LENGTH_VALID = 0x01
_USB_TX_VALIDATION_EVENT_CODE_VALID = 0x02


# Trace the bytes returned by PyUSB for the native Bluetooth Event endpoint
# before hci_transport appends or parses them. Keep this diagnostic local to
# this script so the shared transport behavior remains unchanged.
_native_usb_read_endpoint = transport.NativeUsbTransport._read_endpoint


def _trace_native_usb_read_endpoint(self, endpoint, size, timeout_ms):
    data = _native_usb_read_endpoint(self, endpoint, size, timeout_ms)

    if endpoint is getattr(self, "event_ep", None):
        trace = getattr(self, "_event_read_trace", None)
        if trace is None:
            trace = deque(maxlen=_HOST_EVENT_READ_TRACE_DEPTH)
            self._event_read_trace = trace

        trace.append((time.monotonic(), int(timeout_ms), data))

    return data


transport.NativeUsbTransport._read_endpoint = _trace_native_usb_read_endpoint


def _reserved_event_code(code):
    """Bluetooth HCI reserves 0x00 and 0x3F..0xFE; 0xFF is vendor-specific."""
    return code == 0x00 or (code > 0x3E and code != 0xFF)


def _discard_partial_native_event(hci):
    """Drop bytes already read past a malformed native-USB event boundary."""
    event_rx = getattr(hci.transport, "_event_rx", None)
    if event_rx is not None:
        event_rx.clear()


def _event_read_trace_snapshot(hci):
    trace = getattr(getattr(hci, "transport", None), "_event_read_trace", None)
    return [] if trace is None else list(trace)


def _raw_event_validation(data):
    """Describe one raw Event-IN read without assuming it is one whole HCI Event."""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    expected = data[1] + 2 if len(data) >= 2 else None
    length_valid = expected == len(data) if expected is not None else False
    code_valid = bool(data) and not _reserved_event_code(data[0])
    return {
        "crc": crc,
        "expected": expected,
        "length_valid": length_valid,
        "code_valid": code_valid,
        "first": data[:8],
        "last": data[-8:] if data else b"",
    }


def _print_event_read_trace(label, records, failure_at):
    print("   %s host Event-IN read trace:" % label)
    if not records:
        print("      unavailable")
        return

    for index, (timestamp, timeout_ms, data) in enumerate(records):
        relative_ms = (timestamp - failure_at) * 1000.0
        if data is None:
            print(
                "      [%02u] %+9.3f ms timeout=%u no-data"
                % (index, relative_ms, timeout_ms)
            )
            continue

        check = _raw_event_validation(data)
        expected = (
            "?" if check["expected"] is None else str(check["expected"])
        )
        header = "ok" if check["length_valid"] else "fragment/header-mismatch"
        code = "ok" if check["code_valid"] else "reserved"
        print(
            "      [%02u] %+9.3f ms timeout=%u len=%u expected=%s "
            "header=%s code=%s crc32=%08x first=%s last=%s data=%s"
            % (
                index,
                relative_ms,
                timeout_ms,
                len(data),
                expected,
                header,
                code,
                check["crc"],
                check["first"].hex(" "),
                check["last"].hex(" "),
                data.hex(" "),
            )
        )


def _print_usb_event_ack_trace(label, values):
    print("   %s USB Event-IN ACK trace:" % label)
    if values is None or len(values) < _USB_EVENT_ACK_TRACE_END:
        print("      unavailable")
        return

    records = values[_USB_EVENT_ACK_TRACE_FIRST:_USB_EVENT_ACK_TRACE_END]
    shown = False
    for index, word in enumerate(records):
        if word == 0:
            continue
        shown = True
        chunk = (word >> 24) & 0xFF
        first = word & 0xFF
        second = (word >> 8) & 0xFF
        third = (word >> 16) & 0xFF
        print(
            "      [%02u] chunk=%u first=%02x %02x %02x"
            % (index, chunk, first, second, third)
        )

    if not shown:
        print("      <no payload chunks captured>")


def _read_usb_tx_validation(hci):
    try:
        status, data = hci.command(
            _USB_TX_VALIDATION_OPCODE, allow_fail=True
        )
    except (HciError, HciGone) as err:
        return {"error": str(err), "records": []}

    if status != 0:
        return {"error": "opcode 0xFFF2 status 0x%02X" % status, "records": []}
    if len(data) < 2:
        return {"error": "short 0xFFF2 payload (%u bytes)" % len(data), "records": []}

    version = data[0]
    count = data[1]
    expected = 2 + count * _USB_TX_VALIDATION_RECORD_LEN
    if version != _USB_TX_VALIDATION_VERSION:
        return {
            "error": "0xFFF2 version %u, expected %u"
            % (version, _USB_TX_VALIDATION_VERSION),
            "records": [],
        }
    if len(data) != expected:
        return {
            "error": "0xFFF2 payload %u bytes, expected %u for %u records"
            % (len(data), expected, count),
            "records": [],
        }

    records = []
    offset = 2
    for _ in range(count):
        sequence, length, flags, event_code, crc = struct.unpack_from(
            "<IHBBI", data, offset
        )
        records.append(
            {
                "sequence": sequence,
                "length": length,
                "flags": flags,
                "event_code": event_code,
                "crc": crc,
                "first": bytes(data[offset + 12:offset + 20]),
                "last": bytes(data[offset + 20:offset + 28]),
            }
        )
        offset += _USB_TX_VALIDATION_RECORD_LEN

    return {"error": None, "records": records}


def _classify_host_match(record, raw_records):
    signature = (
        record["length"],
        record["crc"],
        record["first"],
        record["last"],
    )

    for _, _, data in raw_records:
        if data is None:
            continue
        check = _raw_event_validation(data)
        if not check["length_valid"]:
            continue
        raw_signature = (
            len(data),
            check["crc"],
            check["first"],
            check["last"],
        )
        if raw_signature == signature:
            return "complete-match"

    head_match = False
    tail_match = False
    for _, _, data in raw_records:
        if data is None or len(data) >= record["length"] or len(data) < 8:
            continue
        head_match = head_match or data[:8] == record["first"]
        tail_match = tail_match or data[-8:] == record["last"]

    if head_match and tail_match:
        return "fragment-edge-match"
    if tail_match:
        return "tail-fragment-match"
    if head_match:
        return "head-fragment-match"
    return "no-match"


def _print_usb_tx_validation(label, snapshot, raw_records):
    print("   %s pre-USB Event validation:" % label)
    error = snapshot.get("error")
    records = snapshot.get("records", [])
    if error is not None:
        print("      unavailable: %s" % error)
        return
    if not records:
        print("      <no Event-IN packets captured>")
        return

    for index, record in enumerate(records):
        length_ok = bool(record["flags"] & _USB_TX_VALIDATION_LENGTH_VALID)
        code_ok = bool(record["flags"] & _USB_TX_VALIDATION_EVENT_CODE_VALID)
        host = _classify_host_match(record, raw_records)
        print(
            "      [%02u] seq=%u len=%u event=0x%02X header=%s code=%s "
            "crc32=%08x first=%s last=%s host=%s"
            % (
                index,
                record["sequence"],
                record["length"],
                record["event_code"],
                "ok" if length_ok else "BAD",
                "ok" if code_ok else "BAD",
                record["crc"],
                record["first"].hex(" "),
                record["last"].hex(" "),
                host,
            )
        )


def _counter_prefix(values):
    if values is None:
        return None
    return values[:_USB_EVENT_COUNTER_LIMIT]


def _failure_dump(advertiser, scanner, phase_start, failure_at, before_a, before_b):
    # Snapshot host-side EP1 reads before issuing any diagnostic command. Those
    # commands generate their own Event-IN traffic and would overwrite the bytes
    # immediately preceding the malformed packet.
    read_trace_b = _event_read_trace_snapshot(scanner)
    read_trace_a = _event_read_trace_snapshot(advertiser)

    # Read the failing scanner counters first: the DCD ACK ring is only thirteen
    # chunks deep. Then read the pre-USB validation ring. The 0xFFF0 response
    # adds one Event to that eight-record ring, but 0xFFF2 snapshots the history
    # before its own response enters USB.
    after_b = diag._read_counters(scanner)
    tx_validation_b = _read_usb_tx_validation(scanner)
    after_a = diag._read_counters(advertiser)
    tx_validation_a = _read_usb_tx_validation(advertiser)

    print("\n   ===== FAILURE DIAGNOSTICS =====")
    print("   advertiser link_down:", advertiser.link_down)
    print("   scanner/central link_down:", scanner.link_down)
    diag._print_counter_delta(
        advertiser.label, _counter_prefix(before_a), _counter_prefix(after_a)
    )
    diag._print_counter_delta(
        scanner.label, _counter_prefix(before_b), _counter_prefix(after_b)
    )
    _print_event_read_trace(scanner.label, read_trace_b, failure_at)
    _print_event_read_trace(advertiser.label, read_trace_a, failure_at)
    _print_usb_event_ack_trace(advertiser.label, after_a)
    _print_usb_event_ack_trace(scanner.label, after_b)
    _print_usb_tx_validation(advertiser.label, tx_validation_a, read_trace_a)
    _print_usb_tx_validation(scanner.label, tx_validation_b, read_trace_b)
    diag._dump_pending(advertiser)
    diag._dump_pending(scanner)
    advertiser.dump_trace(phase_start, failure_at)
    scanner.dump_trace(phase_start, failure_at)
    print("   ===== END FAILURE DIAGNOSTICS =====\n")


class SafeTraceHci(diag.TraceHci):
    """Trace packets before Hci.on_event can reject malformed event bodies."""

    def _append_trace(self, packet, text=None):
        self.trace.append((time.monotonic(), packet, text or diag._summary(packet)))
        if len(self.trace) > diag.TRACE_LIMIT:
            self.trace = self.trace[-diag.TRACE_LIMIT:]

    def read_wire(self, timeout=1.0):
        try:
            packet = self.transport.read_packet(timeout)
        except transport.TransportGone as err:
            raise HciGone(str(err))
        except transport.TransportError as err:
            raise HciError(str(err))

        if packet is None:
            return None

        kind, code, body = packet

        if kind == H4_EVENT:
            if self.raw:
                print("   rx evt %02x" % code, body.hex(" "))

            if _reserved_event_code(code):
                text = (
                    "MALFORMED reserved HCI Event code 0x%02X len=%u raw=%s"
                    % (code, len(body), body.hex(" "))
                )
                self._append_trace(packet, text)
                # _take_complete() may already have accumulated bytes from
                # later logical Events behind the bogus length. Discard those
                # host-side leftovers so the immediate diagnostics can start
                # at the next native-USB transfer boundary.
                _discard_partial_native_event(self)
                raise HciError(text)

            if code == EVT_NUM_COMPLETED_PACKETS:
                num_handles = body[0] if body else None
                expected = None if num_handles is None else 1 + 4 * num_handles
                if expected is None or len(body) != expected:
                    text = (
                        "MALFORMED Number Of Completed Packets: "
                        "num_handles=%s body_len=%u expected=%s raw=%s"
                        % (
                            "missing" if num_handles is None else str(num_handles),
                            len(body),
                            "unknown" if expected is None else str(expected),
                            body.hex(" "),
                        )
                    )
                    self._append_trace(packet, text)
                    _discard_partial_native_event(self)
                    raise HciError(text)

            self._append_trace(packet)
            self.on_event(code, body)
            return packet

        if self.raw:
            if kind == 0x02:
                print("   rx acl", body[:4].hex(" "), body[4:].hex(" "))
            elif kind == 0x05:
                print("   rx iso", body[:4].hex(" "), body[4:].hex(" "))
            else:
                print("   rx packet type %02x" % kind, body.hex(" "))

        self._append_trace(packet)
        return packet


diag.TraceHci = SafeTraceHci
diag._failure_dump = _failure_dump


if __name__ == "__main__":
    sys.exit(diag.main())

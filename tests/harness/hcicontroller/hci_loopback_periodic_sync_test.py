#!/usr/bin/env python3
"""Stress HCI PRBS loopback while an established periodic sync shares Event-IN."""

import argparse
import struct
import sys
import time

import _bootstrap  # noqa: F401
import hci_loopback_test as loopback
from hcicontroller.hci_pair import HciError, HciGone, prepare_controller
from hcicontroller.hci_transport import SelectionError
from hcicontroller.pair_transport import resolve_pair
from hcicontroller import periodic_features as periodic


H4_COMMAND = 0x01
H4_EVENT = 0x04
EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_LE_META = 0x3E

LE_PERIODIC_SYNC_LOST = 0x10


def _addr(raw):
    return ":".join("%02X" % value for value in reversed(raw))


def _reserved_event_code(code):
    return code == 0x00 or (code > 0x3E and code != 0xFF)


def _new_periodic_stats():
    return {
        "events": 0,
        "legacy": 0,
        "v2": 0,
        "data_bytes": 0,
        "hci_bytes": 0,
        "residual_scan_events": 0,
    }


def _stats_delta(after, before):
    return {key: after[key] - before[key] for key in after}


def _format_periodic_stats(stats, elapsed):
    if elapsed <= 0.0:
        return "periodic events=%u" % stats["events"]
    return (
        "periodic=%u reports (%.1f/s; legacy=%u v2=%u), "
        "data=%.1f kB/s, HCI=%.1f kB/s"
        % (
            stats["events"],
            stats["events"] / elapsed,
            stats["legacy"],
            stats["v2"],
            loopback.rate_kbytes(stats["data_bytes"], elapsed),
            loopback.rate_kbytes(stats["hci_bytes"], elapsed),
        )
    )


def _validate_extended_reports(body):
    if len(body) < 2 or body[0] != periodic.LE_EXTENDED_ADVERTISING_REPORT:
        raise HciError(
            "malformed Extended Advertising Report header: %s" % body.hex(" "))

    count = body[1]
    at = 2
    for index in range(count):
        if len(body) < at + 24:
            raise HciError(
                "malformed Extended Advertising Report %u/%u: "
                "body_len=%u at=%u raw=%s"
                % (index + 1, count, len(body), at, body.hex(" ")))
        data_len = body[at + 23]
        end = at + 24 + data_len
        if len(body) < end:
            raise HciError(
                "truncated Extended Advertising Report %u/%u: "
                "data_len=%u body_len=%u need=%u raw=%s"
                % (index + 1, count, data_len, len(body), end, body.hex(" ")))
        at = end

    if at != len(body):
        raise HciError(
            "Extended Advertising Report trailing bytes: parsed=%u "
            "body_len=%u raw=%s" % (at, len(body), body.hex(" ")))


class PeriodicStressHci(loopback.hci_ble_test.Hci):
    def __init__(self, spec, raw=False):
        super().__init__(spec, raw=raw)
        self.consume_periodic_events = False
        self.accept_residual_scan_events = True
        self.sync_handle = None
        self.periodic_stats = _new_periodic_stats()

    def _consume_periodic_report(self, body):
        subevent = body[0]

        if subevent == periodic.LE_PERIODIC_ADV_REPORT:
            if len(body) < 8:
                raise HciError(
                    "malformed Periodic Advertising Report len=%u raw=%s"
                    % (len(body), body.hex(" ")))
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            data_status = body[6]
            data_len = body[7]
            expected = 8 + data_len
            data = body[8:]
            legacy = True
        elif subevent == periodic.LE_PERIODIC_ADV_REPORT_V2:
            if len(body) < 11:
                raise HciError(
                    "malformed Periodic Advertising Report v2 len=%u raw=%s"
                    % (len(body), body.hex(" ")))
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            data_status = body[9]
            data_len = body[10]
            expected = 11 + data_len
            data = body[11:]
            legacy = False
        else:
            raise AssertionError("not a periodic advertising report")

        if len(body) != expected:
            raise HciError(
                "malformed periodic report length: subevent=0x%02X "
                "body_len=%u expected=%u data_len=%u raw=%s"
                % (subevent, len(body), expected, data_len, body.hex(" ")))
        if handle != self.sync_handle:
            raise HciError(
                "periodic report for unexpected sync handle 0x%04X, expected 0x%04X"
                % (handle, self.sync_handle))
        if data_status != 0:
            raise HciError(
                "periodic report data status=%u handle=0x%04X raw=%s"
                % (data_status, handle, body.hex(" ")))
        if periodic.PERIODIC_MARKER not in data:
            raise HciError(
                "periodic report missing OTA marker: handle=0x%04X data=%s"
                % (handle, data.hex(" ")))

        self.periodic_stats["events"] += 1
        self.periodic_stats["data_bytes"] += data_len
        self.periodic_stats["hci_bytes"] += 2 + len(body)
        self.periodic_stats["legacy" if legacy else "v2"] += 1

    def _consume_async_event(self, packet):
        kind, code, body = packet
        if kind != H4_EVENT:
            raise HciError(
                "unexpected HCI packet type 0x%02X during periodic loopback" % kind)

        if _reserved_event_code(code):
            raise HciError(
                "MALFORMED reserved HCI Event code 0x%02X len=%u raw=%s"
                % (code, len(body), body.hex(" ")))

        if code != EVT_LE_META or not body:
            raise HciError(
                "unexpected asynchronous HCI Event 0x%02X len=%u body=%s"
                % (code, len(body), body.hex(" ")))

        subevent = body[0]
        if subevent in (
            periodic.LE_PERIODIC_ADV_REPORT,
            periodic.LE_PERIODIC_ADV_REPORT_V2,
        ):
            self._consume_periodic_report(body)
            return

        if subevent == periodic.LE_EXTENDED_ADVERTISING_REPORT:
            if not self.accept_residual_scan_events:
                raise HciError(
                    "unexpected Extended Advertising Report after scan disabled: %s"
                    % body.hex(" "))
            _validate_extended_reports(body)
            self.periodic_stats["residual_scan_events"] += 1
            return

        if subevent == LE_PERIODIC_SYNC_LOST:
            handle = (
                struct.unpack("<H", body[1:3])[0] & 0x0FFF
                if len(body) >= 3 else None
            )
            raise HciError(
                "Periodic Sync Lost%s"
                % (
                    " handle=0x%04X" % handle
                    if handle is not None else " (malformed)"
                )
            )

        raise HciError(
            "unexpected LE Meta subevent 0x%02X during periodic loopback: %s"
            % (subevent, body.hex(" ")))

    def command(self, opcode, payload=b"", timeout=3.0, allow_fail=False):
        if not self.consume_periodic_events:
            return super().command(opcode, payload, timeout, allow_fail)

        self.write_packet(
            struct.pack("<BHB", H4_COMMAND, opcode, len(payload)) + payload)
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            if self.pending:
                packet = self.pending.pop(0)
            else:
                packet = self.read_wire(
                    min(0.2, max(0.0, deadline - time.monotonic())))
            if packet is None:
                continue

            kind, code, body = packet

            if kind == H4_EVENT and code == EVT_COMMAND_COMPLETE:
                if len(body) < 4:
                    raise HciError(
                        "malformed Command Complete len=%u raw=%s"
                        % (len(body), body.hex(" ")))
                response_opcode = struct.unpack("<H", body[1:3])[0]
                if response_opcode != opcode:
                    raise HciError(
                        "unexpected Command Complete opcode 0x%04X while "
                        "waiting for 0x%04X" % (response_opcode, opcode))
                status = body[3]
                if status != 0 and not allow_fail:
                    raise HciError(
                        "opcode 0x%04X returned 0x%02X" % (opcode, status))
                return status, body[4:]

            if kind == H4_EVENT and code == EVT_COMMAND_STATUS:
                if len(body) < 4:
                    raise HciError(
                        "malformed Command Status len=%u raw=%s"
                        % (len(body), body.hex(" ")))
                response_opcode = struct.unpack("<H", body[2:4])[0]
                if response_opcode != opcode:
                    raise HciError(
                        "unexpected Command Status opcode 0x%04X while "
                        "waiting for 0x%04X" % (response_opcode, opcode))
                status = body[0]
                if status != 0 and not allow_fail:
                    raise HciError(
                        "opcode 0x%04X status 0x%02X" % (opcode, status))
                return status, b""

            self._consume_async_event(packet)

        raise HciError("no event for opcode 0x%04X" % opcode)


def _establish_periodic_sync(advertiser, scanner, adv_id, adv_type):
    periodic._configure_periodic(advertiser, adv_type)
    periodic._start_periodic(advertiser)

    observed_type, observed_addr, observed_sid, interval = (
        periodic._observe_periodic_advertiser(scanner, timeout=5.0)
    )
    print(
        "SyncInfo observed: addr=%s type=%u sid=%u interval=%u"
        % (_addr(observed_addr), observed_type, observed_sid, interval)
    )

    payload = bytes([0, observed_sid, observed_type]) + observed_addr
    payload += struct.pack("<HHB", 0, 0x0200, 0)
    status, _ = scanner.command(
        periodic.OP_LE_PERIODIC_CREATE_SYNC,
        payload,
        allow_fail=True,
    )
    if status != 0:
        raise HciError(
            "LE Periodic Advertising Create Sync returned 0x%02X" % status)

    sync_handle, _ = periodic._wait_sync_established(scanner, timeout=10.0)
    scanner.sync_handle = sync_handle
    print("Periodic sync established: handle=0x%04X" % sync_handle)

    periodic._wait_periodic_report(
        scanner,
        sync_handle,
        periodic.PERIODIC_MARKER,
        timeout=8.0,
    )
    print("Initial periodic OTA marker: PASS")
    return sync_handle


def main():
    parser = argparse.ArgumentParser(
        description=(
            "PRBS HCI loopback while an established periodic advertising "
            "sync generates unsolicited Event-IN traffic"
        )
    )
    parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto")
    parser.add_argument("--a", help="periodic advertiser selector")
    parser.add_argument("--b", help="scanner/sync receiver selector")
    parser.add_argument("--min-size", type=int, default=0)
    parser.add_argument("--max-size", type=int, default=loopback.MAX_DATA_LEN)
    parser.add_argument("--step", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--verify-error-report", action="store_true")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    if args.min_size < 0 or args.max_size > loopback.MAX_DATA_LEN or \
            args.min_size > args.max_size:
        parser.error(
            "sizes must satisfy 0 <= min <= max <= %u" % loopback.MAX_DATA_LEN)
    if args.step <= 0:
        parser.error("--step must be positive")
    if args.repeat <= 0:
        parser.error("--repeat must be positive")

    try:
        advertiser_spec, scanner_spec = resolve_pair(
            args.a, args.b, kind=args.transport)
    except (HciError, SelectionError) as err:
        print("FAIL:", err, file=sys.stderr)
        return 2

    print("Periodic advertiser:", advertiser_spec)
    print("Scanner/sync receiver:", scanner_spec)
    print("sizes=%u..%u step=%u repeat=%u"
          % (args.min_size, args.max_size, args.step, args.repeat))

    advertiser = None
    scanner = None
    sync_handle = None
    sequence = 0
    passed = 0
    payload_one_way = 0
    hci_bytes = 0
    run_start = None
    run_elapsed = 0.0
    serial_h4 = scanner_spec.kind == "serial"

    try:
        advertiser = loopback.hci_ble_test.Hci(advertiser_spec, raw=args.raw)
        scanner = PeriodicStressHci(scanner_spec, raw=args.raw)

        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)

        if args.verify_error_report:
            check_size = max(1, min(64, args.max_size))
            verify_start = time.perf_counter()
            loopback.run_case(scanner, sequence, check_size, inject_error=True)
            verify_elapsed = time.perf_counter() - verify_start
            print("Controller RX error reporting: PASS (%.3f ms)"
                  % (verify_elapsed * 1000.0))
            sequence = (sequence + 1) & 0xFFFF

        sync_handle = _establish_periodic_sync(
            advertiser, scanner, adv_id, adv_type)
        scanner.consume_periodic_events = True

        while scanner.pending:
            scanner._consume_async_event(scanner.pending.pop(0))
        setup_residual = scanner.periodic_stats["residual_scan_events"]
        if setup_residual:
            print("Setup residual scan events drained: %u" % setup_residual)
        scanner.periodic_stats = _new_periodic_stats()
        scanner.accept_residual_scan_events = False

        print("Periodic sync held; starting PRBS loopback stress")
        run_start = time.perf_counter()

        for sweep in range(args.repeat):
            print("[sweep %u/%u]" % (sweep + 1, args.repeat))
            sweep_start = time.perf_counter()
            before_periodic = dict(scanner.periodic_stats)
            sweep_packets = 0
            sweep_payload_one_way = 0
            sweep_hci_bytes = 0

            for size in range(args.min_size, args.max_size + 1, args.step):
                loopback.run_case(scanner, sequence, size)
                passed += 1
                sweep_packets += 1
                payload_one_way += size
                sweep_payload_one_way += size
                case_hci_bytes = loopback.HCI_BASE_BYTES_PER_CASE + (2 * size)
                hci_bytes += case_hci_bytes
                sweep_hci_bytes += case_hci_bytes
                sequence = (sequence + 1) & 0xFFFF

            sweep_elapsed = time.perf_counter() - sweep_start
            delta = _stats_delta(scanner.periodic_stats, before_periodic)
            print(
                "   PASS through size %u (%u total packets)"
                % (args.max_size, passed)
            )
            print(
                "   "
                + loopback.format_timing(
                    sweep_elapsed,
                    sweep_packets,
                    sweep_payload_one_way,
                    sweep_hci_bytes,
                    serial_h4,
                )
            )
            print("   " + _format_periodic_stats(delta, sweep_elapsed))
            if delta["residual_scan_events"]:
                print(
                    "   residual setup scan events consumed: %u"
                    % delta["residual_scan_events"]
                )

        run_elapsed = time.perf_counter() - run_start

    except (HciError, HciGone) as err:
        elapsed = (
            time.perf_counter() - run_start
            if run_start is not None else 0.0
        )
        print("FAIL:", err)
        print("passed=%u" % passed)
        if scanner is not None:
            print(
                "periodic totals:",
                _format_periodic_stats(scanner.periodic_stats, elapsed),
            )
            print(
                "residual setup scan events=%u"
                % scanner.periodic_stats["residual_scan_events"]
            )
        if passed:
            print("Timing before failure:")
            print(
                "   "
                + loopback.format_timing(
                    elapsed,
                    passed,
                    payload_one_way,
                    hci_bytes,
                    serial_h4,
                )
            )
        return 1
    finally:
        if scanner is not None:
            scanner.consume_periodic_events = False
        cleanup_handle = sync_handle
        if cleanup_handle is None and scanner is not None:
            cleanup_handle = scanner.sync_handle
        if cleanup_handle is not None and scanner is not None:
            periodic._terminate_sync(scanner, cleanup_handle)
        elif scanner is not None:
            periodic._cancel_pending_periodic_sync(scanner)
        if scanner is not None:
            periodic._set_sync_scan(scanner, False, allow_fail=True)
        if advertiser is not None:
            periodic._stop_periodic(advertiser)
        if advertiser is not None:
            advertiser.close()
        if scanner is not None:
            scanner.close()

    print(
        "PASS: %u loopback packets with periodic sync 0x%04X"
        % (passed, sync_handle)
    )
    print("Timing:")
    print(
        "   "
        + loopback.format_timing(
            run_elapsed,
            passed,
            payload_one_way,
            hci_bytes,
            serial_h4,
        )
    )
    print("Periodic traffic:")
    print(
        "   "
        + _format_periodic_stats(scanner.periodic_stats, run_elapsed)
    )
    print(
        "   periodic Event HCI bytes: %u"
        % scanner.periodic_stats["hci_bytes"]
    )
    print("   loopback HCI bytes: %u" % hci_bytes)
    print(
        "   combined observed HCI bytes: %u"
        % (scanner.periodic_stats["hci_bytes"] + hci_bytes)
    )
    print(
        "   residual setup scan events: %u"
        % scanner.periodic_stats["residual_scan_events"]
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

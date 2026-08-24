#!/usr/bin/env python3
"""Stress the HCI PRBS loopback while unsolicited scan events share Event-IN."""

import argparse
import struct
import sys
import time

import _bootstrap  # noqa: F401
import hci_loopback_test as loopback
from hcicontroller.hci_pair import prepare_controller


H4_COMMAND = 0x01
H4_EVENT = 0x04
EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_LE_META = 0x3E
LE_EXTENDED_ADVERTISING_REPORT = 0x0D

OP_LE_SET_EXT_SCAN_PARAMS = 0x2041
OP_LE_SET_EXT_SCAN_ENABLE = 0x2042

SCAN_INTERVAL = 0x0030
SCAN_WINDOW = 0x0030


def _reserved_event_code(code):
    return code == 0x00 or (code > 0x3E and code != 0xFF)


def _new_scan_stats():
    return {
        "events": 0,
        "reports": 0,
        "data_bytes": 0,
        "hci_bytes": 0,
    }


def _scan_stats_delta(after, before):
    return {key: after[key] - before[key] for key in after}


def _parse_extended_reports(body):
    if len(body) < 2 or body[0] != LE_EXTENDED_ADVERTISING_REPORT:
        raise loopback.hci_ble_test.HciError(
            "malformed Extended Advertising Report header: %s" % body.hex(" "))

    report_count = body[1]
    at = 2
    data_bytes = 0

    for report_index in range(report_count):
        if len(body) < at + 24:
            raise loopback.hci_ble_test.HciError(
                "malformed Extended Advertising Report %u/%u: body_len=%u at=%u raw=%s"
                % (report_index + 1, report_count, len(body), at, body.hex(" ")))
        data_len = body[at + 23]
        end = at + 24 + data_len
        if len(body) < end:
            raise loopback.hci_ble_test.HciError(
                "truncated Extended Advertising Report %u/%u: data_len=%u "
                "body_len=%u need=%u raw=%s"
                % (report_index + 1, report_count, data_len,
                   len(body), end, body.hex(" ")))
        data_bytes += data_len
        at = end

    if at != len(body):
        raise loopback.hci_ble_test.HciError(
            "Extended Advertising Report trailing bytes: parsed=%u body_len=%u raw=%s"
            % (at, len(body), body.hex(" ")))

    return report_count, data_bytes


def _format_scan_stats(stats, elapsed):
    if elapsed <= 0.0:
        return "scan events=%u reports=%u" % (stats["events"], stats["reports"])

    return (
        "scan=%u reports (%.1f/s) in %u events, data=%.1f kB/s, HCI=%.1f kB/s"
        % (
            stats["reports"],
            stats["reports"] / elapsed,
            stats["events"],
            loopback.rate_kbytes(stats["data_bytes"], elapsed),
            loopback.rate_kbytes(stats["hci_bytes"], elapsed),
        )
    )


class ScanStressHci(loopback.hci_ble_test.Hci):
    def __init__(self, spec, raw=False):
        super().__init__(spec, raw=raw)
        self.consume_scan_events = False
        self.scan_stats = _new_scan_stats()

    def _consume_scan_event(self, kind, code, body):
        if kind != H4_EVENT:
            raise loopback.hci_ble_test.HciError(
                "unexpected HCI packet type 0x%02X while scanning" % kind)

        if _reserved_event_code(code):
            raise loopback.hci_ble_test.HciError(
                "MALFORMED reserved HCI Event code 0x%02X len=%u raw=%s"
                % (code, len(body), body.hex(" ")))

        if code != EVT_LE_META or not body or \
                body[0] != LE_EXTENDED_ADVERTISING_REPORT:
            raise loopback.hci_ble_test.HciError(
                "unexpected asynchronous HCI Event 0x%02X len=%u body=%s"
                % (code, len(body), body.hex(" ")))

        reports, data_bytes = _parse_extended_reports(body)
        self.scan_stats["events"] += 1
        self.scan_stats["reports"] += reports
        self.scan_stats["data_bytes"] += data_bytes
        self.scan_stats["hci_bytes"] += 2 + len(body)

    def command(self, opcode, payload=b"", timeout=3.0, allow_fail=False):
        if not self.consume_scan_events:
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
                    raise loopback.hci_ble_test.HciError(
                        "malformed Command Complete len=%u raw=%s"
                        % (len(body), body.hex(" ")))
                response_opcode = struct.unpack("<H", body[1:3])[0]
                if response_opcode != opcode:
                    raise loopback.hci_ble_test.HciError(
                        "unexpected Command Complete opcode 0x%04X while waiting for 0x%04X"
                        % (response_opcode, opcode))
                status = body[3]
                if status != 0 and not allow_fail:
                    raise loopback.hci_ble_test.HciError(
                        "opcode 0x%04X returned 0x%02X" % (opcode, status))
                return status, body[4:]

            if kind == H4_EVENT and code == EVT_COMMAND_STATUS:
                if len(body) < 4:
                    raise loopback.hci_ble_test.HciError(
                        "malformed Command Status len=%u raw=%s"
                        % (len(body), body.hex(" ")))
                response_opcode = struct.unpack("<H", body[2:4])[0]
                if response_opcode != opcode:
                    raise loopback.hci_ble_test.HciError(
                        "unexpected Command Status opcode 0x%04X while waiting for 0x%04X"
                        % (response_opcode, opcode))
                status = body[0]
                if status != 0 and not allow_fail:
                    raise loopback.hci_ble_test.HciError(
                        "opcode 0x%04X status 0x%02X" % (opcode, status))
                return status, b""

            self._consume_scan_event(kind, code, body)

        raise loopback.hci_ble_test.HciError(
            "no event for opcode 0x%04X" % opcode)


def _command_ok(hci, opcode, payload=b"", name=None):
    status, data = hci.command(opcode, payload, allow_fail=True)
    if status != 0:
        raise loopback.hci_ble_test.HciError(
            "%s returned 0x%02X" % (name or "opcode 0x%04X" % opcode, status))
    return data


def _start_scan(hci, own_addr_type, scan_mode):
    scan_type = 0x00 if scan_mode == "passive" else 0x01

    params = bytes([own_addr_type, 0x00, 0x01, scan_type])
    params += struct.pack("<HH", SCAN_INTERVAL, SCAN_WINDOW)
    _command_ok(
        hci,
        OP_LE_SET_EXT_SCAN_PARAMS,
        params,
        "LE Set Extended Scan Parameters",
    )

    # Enable=1, duplicate filtering disabled, duration/period zero. This keeps
    # every advertising report flowing for the entire PRBS sweep.
    enable = bytes([0x01, 0x00]) + struct.pack("<HH", 0, 0)
    _command_ok(
        hci,
        OP_LE_SET_EXT_SCAN_ENABLE,
        enable,
        "LE Set Extended Scan Enable",
    )
    hci.consume_scan_events = True


def _stop_scan(hci):
    if hci is None:
        return

    disable = bytes([0x00, 0x00]) + struct.pack("<HH", 0, 0)
    try:
        hci.command(OP_LE_SET_EXT_SCAN_ENABLE, disable, allow_fail=True)
    except (loopback.hci_ble_test.HciError, loopback.hci_ble_test.HciGone):
        pass


def main():
    parser = argparse.ArgumentParser(
        description=(
            "PRBS HCI loopback while passive or active scanning generates "
            "unsolicited Event-IN traffic"
        )
    )
    parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto")
    parser.add_argument("--port", help="UART/CDC H:4 port")
    parser.add_argument("--usb", help="native USB serial number or VID:PID")
    parser.add_argument(
        "--scan", choices=("passive", "active"), default="passive")
    parser.add_argument("--min-size", type=int, default=0)
    parser.add_argument("--max-size", type=int, default=loopback.MAX_DATA_LEN)
    parser.add_argument("--step", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=1,
                        help="number of complete size sweeps")
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
        spec = loopback.resolve_transport(args)
    except loopback.hci_transport.SelectionError as err:
        print("FAIL:", err, file=sys.stderr)
        return 2

    if spec is None:
        print("FAIL: no HciController transport found", file=sys.stderr)
        return 2

    print("Transport:", spec)
    print("scan=%s interval=%u window=%u duplicate_filter=off" %
          (args.scan, SCAN_INTERVAL, SCAN_WINDOW))
    print("sizes=%u..%u step=%u repeat=%u" %
          (args.min_size, args.max_size, args.step, args.repeat))

    serial_h4 = spec.kind == "serial"
    hci = None
    sequence = 0
    passed = 0
    payload_one_way = 0
    hci_bytes = 0
    run_start = None
    run_elapsed = 0.0

    try:
        hci = ScanStressHci(spec, raw=args.raw)
        _, own_addr_type, _ = prepare_controller(hci)

        if args.verify_error_report:
            check_size = max(1, min(64, args.max_size))
            loopback.run_case(hci, sequence, check_size, inject_error=True)
            print("Controller RX error reporting: PASS")
            sequence = (sequence + 1) & 0xFFFF

        _start_scan(hci, own_addr_type, args.scan)
        print("Scanning enabled; starting PRBS loopback stress")

        run_start = time.perf_counter()

        for sweep in range(args.repeat):
            print("[sweep %u/%u]" % (sweep + 1, args.repeat))
            sweep_start = time.perf_counter()
            before_scan = dict(hci.scan_stats)
            sweep_packets = 0
            sweep_payload_one_way = 0
            sweep_hci_bytes = 0

            for size in range(args.min_size, args.max_size + 1, args.step):
                loopback.run_case(hci, sequence, size)
                passed += 1
                sweep_packets += 1
                payload_one_way += size
                sweep_payload_one_way += size
                case_hci_bytes = loopback.HCI_BASE_BYTES_PER_CASE + (2 * size)
                hci_bytes += case_hci_bytes
                sweep_hci_bytes += case_hci_bytes
                sequence = (sequence + 1) & 0xFFFF

            sweep_elapsed = time.perf_counter() - sweep_start
            scan_delta = _scan_stats_delta(hci.scan_stats, before_scan)
            print("   PASS through size %u (%u total packets)" %
                  (args.max_size, passed))
            print("   " + loopback.format_timing(
                sweep_elapsed, sweep_packets, sweep_payload_one_way,
                sweep_hci_bytes, serial_h4))
            print("   " + _format_scan_stats(scan_delta, sweep_elapsed))

        run_elapsed = time.perf_counter() - run_start

    except (loopback.hci_ble_test.HciError,
            loopback.hci_ble_test.HciGone) as err:
        elapsed = (time.perf_counter() - run_start
                   if run_start is not None else 0.0)
        print("FAIL:", err)
        print("passed=%u" % passed)
        if hci is not None:
            print("scan totals:", _format_scan_stats(hci.scan_stats, elapsed))
        if passed:
            print("Timing before failure:")
            print("   " + loopback.format_timing(
                elapsed, passed, payload_one_way, hci_bytes, serial_h4))
        return 1
    finally:
        _stop_scan(hci)
        if hci is not None:
            hci.close()

    print("PASS: %u loopback packets while %s scanning" %
          (passed, args.scan))
    print("Timing:")
    print("   " + loopback.format_timing(
        run_elapsed, passed, payload_one_way, hci_bytes, serial_h4))
    print("Scan traffic:")
    print("   " + _format_scan_stats(hci.scan_stats, run_elapsed))
    print("   scan Event HCI bytes: %u" % hci.scan_stats["hci_bytes"])
    print("   loopback HCI bytes: %u" % hci_bytes)
    print("   combined observed HCI bytes: %u" %
          (hci.scan_stats["hci_bytes"] + hci_bytes))
    return 0


if __name__ == "__main__":
    sys.exit(main())

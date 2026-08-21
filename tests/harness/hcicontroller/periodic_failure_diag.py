#!/usr/bin/env python3
"""Focused failure capture for intermittent periodic-sync and ACL setup faults."""

import argparse
from pathlib import Path
import struct
import sys
import time

_HARNESS_DIR = Path(__file__).resolve().parents[1]
_LIB_DIR = _HARNESS_DIR / "lib"
if str(_HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(_HARNESS_DIR))
if str(_LIB_DIR) not in sys.path:
    sys.path.insert(0, str(_LIB_DIR))

from lib.hci_events import (
    EVT_COMMAND_COMPLETE,
    EVT_COMMAND_STATUS,
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    EVT_NUM_COMPLETED_PACKETS,
    H4_ACL,
    H4_EVENT,
    parse_connection,
    status_text,
)
from lib.hci_pair import Hci, HciError, HciGone, prepare_controller
from lib.hci_transport import SelectionError
from lib.pair_transport import resolve_pair
from lib import periodic_features as periodic


SECOND_EXT_ADV_DATA = b"\x02\x01\x06"
TRACE_LIMIT = 256

LE_NAMES = {
    0x01: "Connection Complete",
    0x03: "Connection Update Complete",
    0x04: "Read Remote Features Complete",
    0x0A: "Enhanced Connection Complete",
    0x0C: "PHY Update Complete",
    0x0D: "Extended Advertising Report",
    0x0E: "Periodic Sync Established",
    0x0F: "Periodic Advertising Report",
    0x10: "Periodic Sync Lost",
    0x18: "PAST Received",
    0x23: "Subrate Change",
    0x24: "Periodic Sync Established v2",
    0x25: "Periodic Advertising Report v2",
    0x26: "PAST Received v2",
    0x29: "Enhanced Connection Complete v2",
}

COUNTER_NAMES = [
    "commands accepted",
    "unknown opcodes",
    "malformed packets",
    "wrong parameter length",
    "handler errors",
    "event backpressure",
    "ACL refused by controller",
    "ISO refused by controller",
    "controller asked for retry",
    "controller queue errors",
    "unknown output types",
    "bad output lengths",
    "command responses deferred",
    "oversize ACL from host",
    "ISO dropped",
    "credit table overflow",
    "ACL taken by controller",
    "ISO taken by controller",
    "H4 bad packet indicator",
    "H4 oversize packet",
    "H4 delivery retried",
    "transport read errors",
    "transport write errors",
    "transport write deferred",
    "transport tx oversize",
    "host packet retried",
    "host packet rejected",
    "controller get errors",
    "controller packet rejected",
    "controller packet unsendable",
    "host over its ACL credits",
    "link table overflow",
    "SDC pool required",
    "SDC pool reserved",
    "PAwR delayed candidates",
    "PAwR delayed handler calls",
    "PAwR synthetic suppressed",
    "PAwR SDC completions",
    "controller ACL packets fetched",
    "host ACL packets accepted",
]


def _addr(raw):
    return ":".join("%02X" % value for value in reversed(raw))


def _summary(packet):
    kind, code, body = packet

    if kind == H4_ACL:
        if len(body) >= 4:
            handle_flags, length = struct.unpack("<HH", body[:4])
            return "ACL handle=0x%04X len=%u" % (handle_flags & 0x0FFF, length)
        return "ACL malformed len=%u" % len(body)

    if kind != H4_EVENT:
        return "packet kind=0x%02X code=%s len=%u" % (kind, code, len(body))

    if code == EVT_COMMAND_COMPLETE and len(body) >= 3:
        opcode = struct.unpack("<H", body[1:3])[0]
        status = body[3] if len(body) >= 4 else 0
        return "Command Complete opcode=0x%04X status=%s" % (
            opcode,
            status_text(status),
        )

    if code == EVT_COMMAND_STATUS and len(body) >= 4:
        opcode = struct.unpack("<H", body[2:4])[0]
        return "Command Status opcode=0x%04X status=%s" % (
            opcode,
            status_text(body[0]),
        )

    if code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4:
        handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        return "Disconnection Complete status=%s handle=0x%04X reason=%s" % (
            status_text(body[0]),
            handle,
            status_text(body[3]),
        )

    if code == EVT_NUM_COMPLETED_PACKETS:
        return "Number Of Completed Packets %s" % body.hex(" ")

    if code == EVT_LE_META and body:
        subevent = body[0]
        name = LE_NAMES.get(subevent, "LE subevent")

        info = parse_connection(body)
        if info is not None:
            status, handle, role, peer, interval, latency, timeout = info
            return (
                "%s status=%s handle=0x%04X role=%u peer=%s "
                "interval=%.2fms latency=%u timeout=%ums"
                % (
                    name,
                    status_text(status),
                    handle,
                    role,
                    _addr(peer),
                    interval * 1.25,
                    latency,
                    timeout * 10,
                )
            )

        if subevent in (
            periodic.LE_PERIODIC_SYNC_ESTABLISHED,
            periodic.LE_PERIODIC_SYNC_ESTABLISHED_V2,
        ) and len(body) >= 4:
            handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
            return "%s status=%s handle=0x%04X len=%u" % (
                name,
                status_text(body[1]),
                handle,
                len(body),
            )

        if subevent == 0x10 and len(body) >= 3:
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            return "%s handle=0x%04X" % (name, handle)

        report = periodic._periodic_report_data(body)
        if report is not None:
            marker = " marker=yes" if periodic.PERIODIC_MARKER in report[3] else ""
            return "%s handle=0x%04X data_len=%u%s" % (
                name,
                report[0],
                len(report[3]),
                marker,
            )

        if subevent == periodic.LE_EXTENDED_ADVERTISING_REPORT:
            reports = periodic._extended_reports(body)
            items = []
            for addr_type, addr, sid, interval, data in reports[:3]:
                items.append(
                    "%s type=%u sid=%u periodic=%u data=%u"
                    % (_addr(addr), addr_type, sid, interval, len(data))
                )
            return "%s count=%u %s" % (
                name,
                len(reports),
                "; ".join(items),
            )

        return "%s (0x%02X) len=%u body=%s" % (
            name,
            subevent,
            len(body),
            body.hex(" "),
        )

    return "Event 0x%02X len=%u body=%s" % (code, len(body), body.hex(" "))


class TraceHci(Hci):
    def __init__(self, spec, label, raw=False):
        super().__init__(spec, raw=raw)
        self.label = label
        self.trace = []
        self.trace_epoch = time.monotonic()

    def read_wire(self, timeout=1.0):
        packet = super().read_wire(timeout)
        if packet is not None:
            self.trace.append((time.monotonic(), packet, _summary(packet)))
            if len(self.trace) > TRACE_LIMIT:
                self.trace = self.trace[-TRACE_LIMIT:]
        return packet

    def dump_trace(self, since, failure_at=None):
        rows = [row for row in self.trace if row[0] >= since]
        print("   %s event trace (%u captured):" % (self.label, len(rows)))
        if not rows:
            print("      <none>")
            return
        for timestamp, _, text in rows:
            phase = "POST" if failure_at is not None and timestamp > failure_at else "    "
            print(
                "      %s +%8.3f ms  %s"
                % (phase, (timestamp - since) * 1000.0, text)
            )


def _read_counters(hci):
    try:
        return hci.read_counters()
    except (HciError, HciGone) as err:
        print("   %s counter read failed: %s" % (hci.label, err))
        return None


def _print_counter_delta(label, before, after):
    print("   %s counter delta:" % label)
    if before is None or after is None:
        print("      unavailable")
        return

    changed = False
    count = min(len(before), len(after))
    for index in range(count):
        if index in (0, 32, 33):
            continue
        delta = (after[index] - before[index]) & 0xFFFFFFFF
        if delta == 0:
            continue
        changed = True
        name = COUNTER_NAMES[index] if index < len(COUNTER_NAMES) else "counter %u" % index
        print("      [%02u] %-32s +%u" % (index, name, delta))

    if not changed:
        print("      no error/flow-control counter changes")


def _dump_pending(hci):
    print("   %s pending queue: %u packet(s)" % (hci.label, len(hci.pending)))
    for packet in hci.pending[-16:]:
        print("      %s" % _summary(packet))


def _failure_dump(advertiser, scanner, phase_start, failure_at, before_a, before_b):
    # Reading counters is intentionally after failure_at. Hci.command() must
    # consume the wire until the counter response appears, so a late async
    # event becomes visible and is marked POST in the trace instead of being
    # silently lost to the diagnostic.
    after_a = _read_counters(advertiser)
    after_b = _read_counters(scanner)

    print("\n   ===== FAILURE DIAGNOSTICS =====")
    print("   advertiser link_down:", advertiser.link_down)
    print("   scanner/central link_down:", scanner.link_down)
    _print_counter_delta(advertiser.label, before_a, after_a)
    _print_counter_delta(scanner.label, before_b, after_b)
    _dump_pending(advertiser)
    _dump_pending(scanner)
    advertiser.dump_trace(phase_start, failure_at)
    scanner.dump_trace(phase_start, failure_at)
    print("   ===== END FAILURE DIAGNOSTICS =====\n")


def _configure_connectable(advertiser, own_addr_type):
    periodic._configure_ext_set(
        advertiser,
        periodic.ADV_HANDLE_CONNECTABLE,
        own_addr_type,
        0x0001,
        periodic.ADV_SID_CONNECTABLE,
        SECOND_EXT_ADV_DATA,
    )


def _configure_second_ext_set(advertiser, own_addr_type):
    periodic._configure_ext_set(
        advertiser,
        periodic.ADV_HANDLE_PERIODIC,
        own_addr_type,
        0x0000,
        periodic.ADV_SID_PERIODIC,
        SECOND_EXT_ADV_DATA,
    )


def _disable_all_ext(advertiser):
    try:
        periodic._ext_adv_enable(
            advertiser,
            periodic.ADV_HANDLE_CONNECTABLE,
            False,
        )
    except (HciError, HciGone):
        pass


def _wait_acl_pair_diagnostic(central, peripheral, timeout=10.0):
    handles = {"central": None, "peripheral": None}
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline and None in handles.values():
        for label, hci in (("central", central), ("peripheral", peripheral)):
            packet = hci.read_packet(0.05)
            if packet is None:
                continue

            kind, code, body = packet
            if kind == H4_EVENT and code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4:
                handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
                raise HciError(
                    "%s disconnected during ACL establishment: status=%s "
                    "handle=0x%04X reason=%s"
                    % (label, status_text(body[0]), handle, status_text(body[3]))
                )

            if kind != H4_EVENT or code != EVT_LE_META:
                continue

            info = parse_connection(body)
            if info is None:
                continue

            status, handle, role, peer, interval, _, _ = info
            print(
                "      %s Connection Complete status=%s handle=0x%04X "
                "role=%u peer=%s interval=%.2f ms"
                % (
                    label,
                    status_text(status),
                    handle,
                    role,
                    _addr(peer),
                    interval * 1.25,
                )
            )

            if status != 0:
                raise HciError(
                    "%s connection failed: %s" % (label, status_text(status))
                )

            expected_role = 0 if label == "central" else 1
            if role != expected_role:
                raise HciError(
                    "%s reported role %u, expected %u"
                    % (label, role, expected_role)
                )
            handles[label] = handle

    if None in handles.values():
        raise HciError(
            "ACL establishment timeout: central=%s peripheral=%s "
            "central.link_down=%s peripheral.link_down=%s"
            % (
                "0x%04X" % handles["central"] if handles["central"] is not None else "missing",
                "0x%04X" % handles["peripheral"] if handles["peripheral"] is not None else "missing",
                central.link_down,
                peripheral.link_down,
            )
        )

    return handles["central"], handles["peripheral"]


def _monitor_acl(central, peripheral, central_handle, peripheral_handle, duration):
    started = time.monotonic()
    deadline = started + duration

    while time.monotonic() < deadline:
        for label, hci, expected in (
            ("central", central, central_handle),
            ("peripheral", peripheral, peripheral_handle),
        ):
            packet = hci.read_packet(0.02)
            if packet is None:
                continue
            kind, code, body = packet
            if kind != H4_EVENT or code != EVT_DISCONNECTION_COMPLETE or len(body) < 4:
                continue
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            if handle != expected:
                continue
            raise HciError(
                "%s disconnected %.3f s after both ACL events: "
                "status=%s handle=0x%04X reason=%s"
                % (
                    label,
                    time.monotonic() - started,
                    status_text(body[0]),
                    handle,
                    status_text(body[3]),
                )
            )


def _wait_sync_diagnostic(scanner, timeout=10.0):
    deadline = time.monotonic() + timeout
    counts = {}

    while time.monotonic() < deadline:
        packet = scanner.read_packet(0.1)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META or not body:
            continue

        subevent = body[0]
        counts[subevent] = counts.get(subevent, 0) + 1

        if subevent not in (
            periodic.LE_PERIODIC_SYNC_ESTABLISHED,
            periodic.LE_PERIODIC_SYNC_ESTABLISHED_V2,
        ):
            continue

        if len(body) < 4:
            raise HciError("malformed Periodic Sync Established: %s" % body.hex(" "))

        status = body[1]
        handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        if status != 0:
            raise HciError(
                "Periodic Sync Established failed: status=%s handle=0x%04X"
                % (status_text(status), handle)
            )
        return handle, counts

    summary = " ".join(
        "0x%02X=%u" % (subevent, count)
        for subevent, count in sorted(counts.items())
    ) or "none"
    raise HciError(
        "timed out waiting for Periodic Sync Established; LE subevents seen: %s"
        % summary
    )


def _run_sync(advertiser_spec, scanner_spec, raw):
    advertiser = TraceHci(advertiser_spec, "advertiser", raw=raw)
    scanner = TraceHci(scanner_spec, "scanner", raw=raw)
    sync_handle = None
    failure = None
    failure_at = None
    phase_start = None
    before_a = before_b = None

    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)
        periodic._configure_periodic(advertiser, adv_type)
        periodic._start_periodic(advertiser)

        before_a = _read_counters(advertiser)
        before_b = _read_counters(scanner)
        phase_start = time.monotonic()

        observed_type, observed_addr, observed_sid, interval = (
            periodic._observe_periodic_advertiser(scanner, timeout=5.0)
        )
        print(
            "      SyncInfo observed: addr=%s type=%u sid=%u interval=%u"
            % (_addr(observed_addr), observed_type, observed_sid, interval)
        )

        payload = bytes([0, observed_sid, observed_type]) + observed_addr
        payload += struct.pack("<HHB", 0, 0x0200, 0)

        create_at = time.monotonic()
        status, _ = scanner.command(
            periodic.OP_LE_PERIODIC_CREATE_SYNC,
            payload,
            allow_fail=True,
        )
        print(
            "      Create Sync Command Status: %s at +%.3f ms"
            % (status_text(status), (time.monotonic() - create_at) * 1000.0)
        )
        if status != 0:
            raise HciError("LE Periodic Advertising Create Sync returned %s" % status_text(status))

        sync_handle, counts = _wait_sync_diagnostic(scanner, timeout=10.0)
        print(
            "      Sync Established: handle=0x%04X after %.3f s; LE counts=%s"
            % (
                sync_handle,
                time.monotonic() - create_at,
                " ".join(
                    "0x%02X=%u" % item for item in sorted(counts.items())
                ),
            )
        )

        periodic._set_sync_scan(scanner, False, allow_fail=True)
        periodic._wait_periodic_report(
            scanner,
            sync_handle,
            periodic.PERIODIC_MARKER,
            timeout=8.0,
        )
        print("      OTA marker received")
        return True

    except (HciError, HciGone) as err:
        failure = str(err)
        failure_at = time.monotonic()
        print("      FAIL:", failure)
        if phase_start is not None:
            _failure_dump(
                advertiser,
                scanner,
                phase_start,
                failure_at,
                before_a,
                before_b,
            )
        return False

    finally:
        if sync_handle is not None:
            periodic._terminate_sync(scanner, sync_handle)
        else:
            periodic._cancel_pending_periodic_sync(scanner)
        periodic._set_sync_scan(scanner, False, allow_fail=True)
        periodic._stop_periodic(advertiser)
        advertiser.close()
        scanner.close()


def _run_acl_two_sets(advertiser_spec, central_spec, raw, idle_seconds):
    advertiser = TraceHci(advertiser_spec, "advertiser/peripheral", raw=raw)
    central = TraceHci(central_spec, "central", raw=raw)
    central_handle = peripheral_handle = None
    phase_start = None
    failure_at = None
    before_a = before_b = None

    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        _, central_type, _ = prepare_controller(central)

        _configure_second_ext_set(advertiser, adv_type)
        _configure_connectable(advertiser, adv_type)
        periodic._ext_adv_enable(advertiser, periodic.ADV_HANDLE_PERIODIC, True)
        periodic._ext_adv_enable(advertiser, periodic.ADV_HANDLE_CONNECTABLE, True)

        before_a = _read_counters(advertiser)
        before_b = _read_counters(central)
        phase_start = time.monotonic()

        periodic._start_extended_connection(
            central,
            central_type,
            adv_id,
            adv_type,
        )
        central_handle, peripheral_handle = _wait_acl_pair_diagnostic(
            central,
            advertiser,
        )
        print(
            "      both ACL events received: central=0x%04X peripheral=0x%04X"
            % (central_handle, peripheral_handle)
        )

        _monitor_acl(
            central,
            advertiser,
            central_handle,
            peripheral_handle,
            idle_seconds,
        )
        print("      ACL survived %.1f s" % idle_seconds)
        return True

    except (HciError, HciGone) as err:
        failure_at = time.monotonic()
        print("      FAIL:", err)
        if phase_start is not None:
            _failure_dump(
                advertiser,
                central,
                phase_start,
                failure_at,
                before_a,
                before_b,
            )
        return False

    finally:
        if central_handle is not None:
            try:
                central.command(
                    0x0406,
                    struct.pack("<HB", central_handle, 0x13),
                    allow_fail=True,
                )
            except (HciError, HciGone):
                pass
        _disable_all_ext(advertiser)
        advertiser.close()
        central.close()


def main():
    parser = argparse.ArgumentParser(
        description="Stop on the first intermittent periodic/ACL failure and dump diagnostics"
    )
    parser.add_argument(
        "mode",
        choices=("sync", "acl-two-sets"),
        help=(
            "sync: periodic sync only, no ACL; "
            "acl-two-sets: Case-B ACL with two extended advertising sets, no periodic"
        ),
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "serial", "usb"),
        default="auto",
    )
    parser.add_argument("--a", help="advertiser/peripheral selector")
    parser.add_argument("--b", help="scanner/central selector")
    parser.add_argument("--repeat", type=int, default=50)
    parser.add_argument("--idle-seconds", type=float, default=3.0)
    parser.add_argument("--raw", action="store_true")
    parser.add_argument(
        "--keep-going",
        action="store_true",
        help="continue after failures instead of stopping on the first one",
    )
    args = parser.parse_args()

    if args.repeat <= 0:
        parser.error("--repeat must be positive")
    if args.idle_seconds <= 0:
        parser.error("--idle-seconds must be positive")

    try:
        advertiser_spec, peer_spec = resolve_pair(
            args.a,
            args.b,
            kind=args.transport,
        )
    except (HciError, SelectionError) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    print("Mode:", args.mode)
    print("Advertiser/peripheral:", advertiser_spec)
    print("Scanner/central:      ", peer_spec)
    print("repeat=%u" % args.repeat)

    failures = 0
    for iteration in range(1, args.repeat + 1):
        print("\n[%s %u/%u]" % (args.mode, iteration, args.repeat))
        if args.mode == "sync":
            ok = _run_sync(advertiser_spec, peer_spec, args.raw)
        else:
            ok = _run_acl_two_sets(
                advertiser_spec,
                peer_spec,
                args.raw,
                args.idle_seconds,
            )

        if not ok:
            failures += 1
            if not args.keep_going:
                print("Stopped on first failure.")
                break

    print("\n================================")
    print("mode=%s failures=%u" % (args.mode, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

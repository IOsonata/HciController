#!/usr/bin/env python3
"""Validate periodic advertising/ACL ordering with mandatory OTA proof."""

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

from lib.hci_pair import (
    EVT_DISCONNECTION_COMPLETE,
    H4_EVENT,
    Hci,
    HciError,
    HciGone,
    disconnect_acl_pair,
    prepare_controller,
    wait_acl_pair,
)
from lib.hci_transport import SelectionError
from lib.pair_transport import resolve_pair
from lib import periodic_features as periodic
from lib.results import ResultBook

LE_PERIODIC_SYNC_LOST = 0x10
SECOND_EXT_ADV_DATA = b"\x02\x01\x06"

CASE_DESCRIPTIONS = {
    "A": "connectable extended advertising only",
    "B": "second extended advertising set, no periodic train",
    "C": "periodic OTA proven before ACL",
    "D": "ACL first, then immediate periodic OTA proof",
    "E": "ACL first, delay, then periodic OTA proof",
}


def _parse_cases(value):
    cases = []
    for item in value.split(","):
        case = item.strip().upper()
        if not case:
            continue
        if case not in CASE_DESCRIPTIONS:
            raise argparse.ArgumentTypeError(
                "unknown case %s; choose from A,B,C,D,E" % case
            )
        if case not in cases:
            cases.append(case)
    if not cases:
        raise argparse.ArgumentTypeError("at least one case is required")
    return cases


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


def _enable_ext_set(advertiser, handle):
    periodic._ext_adv_enable(advertiser, handle, True)


def _disable_all_ext_sets(advertiser):
    try:
        periodic._ext_adv_enable(
            advertiser,
            periodic.ADV_HANDLE_CONNECTABLE,
            False,
        )
    except (HciError, HciGone):
        pass


def _establish_acl(central, advertiser, central_type, adv_id, adv_type):
    periodic._start_extended_connection(
        central,
        central_type,
        adv_id,
        adv_type,
    )
    return wait_acl_pair(central, advertiser)


def _prove_periodic_ota(advertiser, scanner, adv_id, adv_type):
    periodic._create_periodic_sync(scanner, adv_id, adv_type)
    sync_handle, _ = periodic._wait_sync_established(scanner)
    periodic._wait_periodic_report(
        scanner,
        sync_handle,
        periodic.PERIODIC_MARKER,
    )
    return sync_handle


def _disconnection(packet, expected_handle):
    kind, code, body = packet
    if (
        kind != H4_EVENT
        or code != EVT_DISCONNECTION_COMPLETE
        or len(body) < 4
    ):
        return None

    status = body[0]
    handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
    reason = body[3]
    if handle != expected_handle:
        return None
    return status, handle, reason


def _periodic_sync_lost(packet, sync_handle):
    kind, code, body = packet
    if (
        sync_handle is None
        or kind != H4_EVENT
        or code != periodic.EVT_LE_META
        or len(body) < 3
        or body[0] != LE_PERIODIC_SYNC_LOST
    ):
        return False
    got = struct.unpack("<H", body[1:3])[0] & 0x0FFF
    return got == sync_handle


def _periodic_marker(packet, sync_handle):
    kind, code, body = packet
    if (
        sync_handle is None
        or kind != H4_EVENT
        or code != periodic.EVT_LE_META
    ):
        return False

    parsed = periodic._periodic_report_data(body)
    return (
        parsed is not None
        and parsed[0] == sync_handle
        and periodic.PERIODIC_MARKER in parsed[3]
    )


def _monitor(
    central,
    advertiser,
    central_handle,
    advertiser_handle,
    sync_handle,
    duration,
    acl_time,
):
    periodic_reports = 0
    deadline = time.monotonic() + duration

    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        timeout = min(0.02, max(0.0, remaining))

        for label, hci, handle in (
            ("central", central, central_handle),
            ("advertiser/peripheral", advertiser, advertiser_handle),
        ):
            packet = hci.read_packet(timeout)
            if packet is None:
                continue

            disconnected = _disconnection(packet, handle)
            if disconnected is not None:
                status, got, reason = disconnected
                return {
                    "ok": False,
                    "periodic_reports": periodic_reports,
                    "detail": (
                        "%s disconnected, status=0x%02X handle=0x%04X "
                        "reason=0x%02X observed %.3f s after ACL establishment"
                        % (
                            label,
                            status,
                            got,
                            reason,
                            time.monotonic() - acl_time,
                        )
                    ),
                }

            if hci is central:
                if _periodic_sync_lost(packet, sync_handle):
                    return {
                        "ok": False,
                        "periodic_reports": periodic_reports,
                        "detail": (
                            "periodic sync 0x%04X lost while ACL remained under test"
                            % sync_handle
                        ),
                    }
                if _periodic_marker(packet, sync_handle):
                    periodic_reports += 1

    if sync_handle is not None and periodic_reports == 0:
        return {
            "ok": False,
            "periodic_reports": 0,
            "detail": "ACL survived but no periodic OTA marker arrived during idle window",
        }

    return {
        "ok": True,
        "periodic_reports": periodic_reports,
        "detail": (
            "ACL alive %.1f s%s"
            % (
                duration,
                (
                    "; periodic OTA reports=%u" % periodic_reports
                    if sync_handle is not None
                    else ""
                ),
            )
        ),
    }


def _run_iteration(case, advertiser_spec, central_spec, raw, idle_seconds, delay):
    advertiser = Hci(advertiser_spec, raw=raw)
    central = Hci(central_spec, raw=raw)

    sync_handle = None
    central_handle = None
    advertiser_handle = None
    periodic_configured = False

    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        _, central_type, _ = prepare_controller(central)

        if case == "A":
            _configure_connectable(advertiser, adv_type)
            _enable_ext_set(advertiser, periodic.ADV_HANDLE_CONNECTABLE)

        elif case == "B":
            _configure_second_ext_set(advertiser, adv_type)
            _configure_connectable(advertiser, adv_type)
            _enable_ext_set(advertiser, periodic.ADV_HANDLE_PERIODIC)
            _enable_ext_set(advertiser, periodic.ADV_HANDLE_CONNECTABLE)

        elif case == "C":
            periodic._configure_periodic(advertiser, adv_type)
            periodic_configured = True
            periodic._start_periodic(advertiser)

            sync_handle = _prove_periodic_ota(
                advertiser,
                central,
                adv_id,
                adv_type,
            )
            print(
                "      periodic pre-ACL: sync 0x%04X + OTA marker confirmed"
                % sync_handle
            )

            _configure_connectable(advertiser, adv_type)
            _enable_ext_set(advertiser, periodic.ADV_HANDLE_CONNECTABLE)

        elif case in ("D", "E"):
            periodic._configure_periodic(advertiser, adv_type)
            periodic_configured = True
            _configure_connectable(advertiser, adv_type)
            _enable_ext_set(advertiser, periodic.ADV_HANDLE_CONNECTABLE)

        central_handle, advertiser_handle = _establish_acl(
            central,
            advertiser,
            central_type,
            adv_id,
            adv_type,
        )
        acl_time = time.monotonic()

        print(
            "      ACL central=0x%04X peripheral=0x%04X"
            % (central_handle, advertiser_handle)
        )

        if case in ("D", "E"):
            if case == "E":
                pre_periodic = _monitor(
                    central,
                    advertiser,
                    central_handle,
                    advertiser_handle,
                    None,
                    delay,
                    acl_time,
                )
                if not pre_periodic["ok"]:
                    pre_periodic["detail"] = (
                        "before periodic enable: " + pre_periodic["detail"]
                    )
                    return pre_periodic

            periodic._start_periodic(advertiser)
            sync_handle = _prove_periodic_ota(
                advertiser,
                central,
                adv_id,
                adv_type,
            )
            print(
                "      periodic post-ACL: sync 0x%04X + OTA marker confirmed "
                "%.3f s after ACL"
                % (sync_handle, time.monotonic() - acl_time)
            )

        return _monitor(
            central,
            advertiser,
            central_handle,
            advertiser_handle,
            sync_handle,
            idle_seconds,
            acl_time,
        )

    except (HciError, HciGone) as err:
        return {
            "ok": False,
            "periodic_reports": 0,
            "detail": str(err),
        }

    finally:
        if central_handle is not None:
            disconnect_acl_pair(
                central,
                advertiser,
                central_handle,
                advertiser_handle,
            )

        if sync_handle is not None:
            periodic._terminate_sync(central, sync_handle)
        else:
            periodic._cancel_pending_periodic_sync(central)

        try:
            periodic._set_sync_scan(central, False, allow_fail=True)
        except (HciError, HciGone):
            pass

        if periodic_configured:
            periodic._stop_periodic(advertiser)
        else:
            _disable_all_ext_sets(advertiser)

        advertiser.close()
        central.close()


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Validate periodic-advertising/ACL ordering with real over-the-air "
            "periodic proof"
        )
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "serial", "usb"),
        default="auto",
        help="host transport; auto prefers native USB when present",
    )
    parser.add_argument(
        "--a",
        help="advertiser/peripheral selector: serial device or native USB serial number",
    )
    parser.add_argument(
        "--b",
        help="scanner/central selector: serial device or native USB serial number",
    )
    parser.add_argument(
        "--cases",
        type=_parse_cases,
        default=list(CASE_DESCRIPTIONS),
        help="comma-separated matrix cases (default: A,B,C,D,E)",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=20,
        help="iterations per selected case (default: %(default)s)",
    )
    parser.add_argument(
        "--idle-seconds",
        type=float,
        default=3.0,
        help="ACL/periodic coexistence observation window (default: %(default)s)",
    )
    parser.add_argument(
        "--post-acl-delay",
        type=float,
        default=0.5,
        help="case E delay before periodic enable (default: %(default)s)",
    )
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="swap the selected dongles before assigning advertiser/central roles",
    )
    parser.add_argument("--raw", action="store_true", help="show raw HCI traffic")
    args = parser.parse_args()

    if args.repeat <= 0:
        parser.error("--repeat must be positive")
    if args.idle_seconds <= 0:
        parser.error("--idle-seconds must be positive")
    if args.post_acl_delay < 0:
        parser.error("--post-acl-delay must not be negative")

    try:
        advertiser_spec, central_spec = resolve_pair(
            args.a,
            args.b,
            kind=args.transport,
        )
    except (HciError, SelectionError) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    if args.reverse:
        advertiser_spec, central_spec = central_spec, advertiser_spec

    print("Advertiser/peripheral:", advertiser_spec)
    print("Scanner/central:      ", central_spec)
    print(
        "Matrix cases: %s; repeat=%u; idle=%.1f s; case-E delay=%.3f s"
        % (
            ",".join(args.cases),
            args.repeat,
            args.idle_seconds,
            args.post_acl_delay,
        )
    )

    book = ResultBook("Periodic advertising + ACL OTA ordering validation")

    for case in args.cases:
        failures = []
        total_reports = 0

        print()
        print(
            "Case %s: %s"
            % (case, CASE_DESCRIPTIONS[case])
        )

        for iteration in range(1, args.repeat + 1):
            print("  [%s %u/%u]" % (case, iteration, args.repeat))
            result = _run_iteration(
                case,
                advertiser_spec,
                central_spec,
                args.raw,
                args.idle_seconds,
                args.post_acl_delay,
            )

            total_reports += result["periodic_reports"]

            if result["ok"]:
                print("      PASS: %s" % result["detail"])
            else:
                print("      FAIL: %s" % result["detail"])
                failures.append((iteration, result["detail"]))

        if failures:
            detail = "%u/%u failed" % (len(failures), args.repeat)
            first_iteration, first_detail = failures[0]
            detail += "; first failure run %u: %s" % (
                first_iteration,
                first_detail,
            )
            book.failed(
                "Ordering matrix",
                "Case %s - %s" % (case, CASE_DESCRIPTIONS[case]),
                detail,
            )
        else:
            detail = "%u/%u passed" % (args.repeat, args.repeat)
            if case in ("C", "D", "E"):
                detail += "; periodic reports=%u" % total_reports
            book.passed(
                "Ordering matrix",
                "Case %s - %s" % (case, CASE_DESCRIPTIONS[case]),
                detail,
            )

    book.print_report()
    return book.exit_code()


if __name__ == "__main__":
    sys.exit(main())

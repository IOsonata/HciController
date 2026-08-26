#!/usr/bin/env python3
"""Two-HciController release acceptance harness.

Every advertised Release-1 capability is exercised positively. A procedure
that cannot be completed is FAIL, not a hidden skip.
"""

import argparse
from pathlib import Path
import subprocess
import sys

import _bootstrap  # noqa: F401
from hcicontroller import bis_features
from hcicontroller import hci_cis_pair_test as cis_pair
from hcicontroller import stress_features
from hcicontroller.connected_features_pair import run_connected_feature_phase, run_subrate_phase
from hcicontroller.core_advanced import run_core_advanced_phase
from hcicontroller.hci_cis_usb_pair_test import NativeIsoHci
from hcicontroller.hci_pair import Hci, HciError
from hcicontroller.hci_transport import SelectionError
from hcicontroller.pair_transport import (
    bulk_spec,
    resolve_pair,
    spec_selector,
    transport_cli_args,
)
from hcicontroller.periodic_features import run_past_phase, run_pawr_phase, run_periodic_sync_phase
from hcicontroller.profile import read_controller_capabilities
from hcicontroller.recovery_features import run_recovery_phase
from hcicontroller.results import FAIL, ResultBook
from hcicontroller.stress_features import DEFAULT_STRESS_COUNT
from pair_smoke_test import check_profile

_REPO_ROOT = Path(__file__).resolve().parents[3]
_ESTABLISHMENT_ATTEMPTS = 3
_ESTABLISHMENT_REASON = 0x3E


def run_profile_phase(book, spec_a, spec_b, raw):
    a = Hci(spec_a, raw=raw)
    b = Hci(spec_b, raw=raw)
    try:
        try:
            check_profile(book, "A", read_controller_capabilities(a))
        except Exception as err:
            book.failed("Profile", "A capability read", str(err))
        try:
            check_profile(book, "B", read_controller_capabilities(b))
        except Exception as err:
            book.failed("Profile", "B capability read", str(err))
    finally:
        a.close()
        b.close()


def with_pair(spec_a, spec_b, raw, callback, *args):
    a = Hci(spec_a, raw=raw)
    b = Hci(spec_b, raw=raw)
    try:
        callback(*args, a, b)
    finally:
        a.close()
        b.close()


def _link_establishment_failed(hci):
    down = getattr(hci, "link_down", None)
    return (
        down is not None
        and len(down) >= 3
        and down[2] == _ESTABLISHMENT_REASON
    )


def _first_failure(book):
    for result in book.results:
        if result.status == FAIL:
            return result
    return None


def _retryable_establishment_attempt(book, a, b):
    """Return the first failure when this attempt ended in LE 0x3E."""
    failure = _first_failure(book)
    if failure is None:
        return None

    detail = failure.detail or ""
    explicit_status = (
        "connection failed: 0x3E" in detail
        or "Connection Failed To Be Established" in detail
    )
    link_down = _link_establishment_failed(a) or _link_establishment_failed(b)

    if explicit_status:
        return failure

    if not link_down:
        return None

    if "timed out waiting for both ACL connection events" in detail:
        return failure
    if "no LE Read Remote Features Complete event" in detail:
        return failure

    # Recovery prefixes the connection error with the step name.
    if "ACL recovery after advanced-state resets" in detail:
        return failure

    return None


def with_pair_establishment_retry(
    spec_a,
    spec_b,
    raw,
    callback,
    book,
    label,
    attempts=_ESTABLISHMENT_ATTEMPTS,
):
    """Retry only a fresh-pair attempt that demonstrably ended with LE 0x3E."""
    for attempt in range(1, attempts + 1):
        attempt_book = ResultBook(book.title)
        a = Hci(spec_a, raw=raw)
        b = Hci(spec_b, raw=raw)
        retry_failure = None
        try:
            callback(attempt_book, label, a, b)
            retry_failure = _retryable_establishment_attempt(
                attempt_book, a, b
            )
        finally:
            a.close()
            b.close()

        if retry_failure is not None and attempt < attempts:
            print(
                "HOST-RETRY: %s attempt %u/%u ended in LE 0x3E; "
                "retrying the complete phase"
                % (label, attempt, attempts)
            )
            continue

        book.results.extend(attempt_book.results)

        if retry_failure is None and attempt > 1:
            print(
                "HOST-RETRY: %s recovered on attempt %u/%u"
                % (label, attempt, attempts)
            )
        elif retry_failure is not None:
            print(
                "HOST-RETRY: %s still failed with LE 0x3E after %u attempts"
                % (label, attempts)
            )
        return


def run_probe(book, label, spec, raw):
    script = Path(__file__).resolve().parent / "probe_test.py"
    try:
        command = [sys.executable, str(script)] + transport_cli_args(spec)
    except SelectionError as err:
        book.failed("Broad command probe", label, str(err))
        return
    if raw:
        command.append("--raw")
    command += ["probe", "--consent", "--verbose"]
    result = subprocess.run(command, cwd=str(_REPO_ROOT))
    if result.returncode == 0:
        book.passed("Broad command probe", label)
    else:
        book.failed("Broad command probe", label,
                    "exit %d" % result.returncode)


def _run_mixed_cis(spec_a, spec_b, raw):
    phase_a = bulk_spec(spec_a)
    phase_b = bulk_spec(spec_b)
    old_iso_hci = cis_pair.IsoHci
    old_find = cis_pair.find_hci_ports
    old_argv = sys.argv
    cis_pair.IsoHci = NativeIsoHci
    cis_pair.find_hci_ports = lambda: [phase_a, phase_b]
    sys.argv = [old_argv[0]] + (["--raw"] if raw else [])
    try:
        return cis_pair.main()
    finally:
        sys.argv = old_argv
        cis_pair.find_hci_ports = old_find
        cis_pair.IsoHci = old_iso_hci


def run_cis(book, spec_a, spec_b, raw):
    if spec_a.kind != spec_b.kind:
        result = _run_mixed_cis(spec_a, spec_b, raw)
        if result == 0:
            book.passed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO")
        else:
            book.failed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO",
                        "exit %d" % result)
        return

    if spec_a.kind == "usb":
        script = Path(__file__).resolve().parent / "cis_usb_pair_test.py"
    else:
        script = Path(__file__).resolve().parent / "cis_pair_test.py"

    try:
        central = spec_selector(spec_a)
        peripheral = spec_selector(spec_b)
    except SelectionError as err:
        book.failed(
            "ISO", "CIS Central/Peripheral + bidirectional HCI ISO", str(err)
        )
        return

    command = [
        sys.executable,
        str(script),
        "--central", central,
        "--peripheral", peripheral,
    ]
    if raw:
        command.append("--raw")
    result = subprocess.run(command, cwd=str(_REPO_ROOT))
    if result.returncode == 0:
        book.passed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO")
    else:
        book.failed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO",
                    "exit %d" % result.returncode)


def run_iso_phase(module, callback, book, label, spec_a, spec_b,
                  raw=False, **kwargs):
    """Run an ISO phase over serial H:4, native USB, or a mixed pair."""
    old_iso_hci = module.IsoHci
    phase_a = bulk_spec(spec_a)
    phase_b = bulk_spec(spec_b)
    if spec_a.kind == "usb" or spec_b.kind == "usb":
        module.IsoHci = NativeIsoHci
    try:
        callback(
            book, label, phase_a, phase_b,
            raw=raw, **kwargs
        )
    finally:
        module.IsoHci = old_iso_hci


def main():
    parser = argparse.ArgumentParser(
        description="Run release-strict validation with two HciController dongles"
    )
    parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto",
        help="host transport; auto allows serial H:4 or native USB per controller",
    )
    parser.add_argument(
        "--a",
        help="first controller selector: serial device or native USB serial number",
    )
    parser.add_argument(
        "--b",
        help="second controller selector: serial device or native USB serial number",
    )
    parser.add_argument("--raw", action="store_true", help="show raw HCI traffic")
    parser.add_argument("--only-pawr", action="store_true",
                        help="run only the PAwR advertiser/scanner phase")
    parser.add_argument("--only-recovery", action="store_true",
                        help="run only the advanced-state recovery phase")
    parser.add_argument(
        "--only-bis-recovery",
        action="store_true",
        help="run only BIS teardown followed immediately by legacy recovery",
    )
    parser.add_argument("--skip-probe", action="store_true",
                        help="skip the per-dongle broad command probes")
    parser.add_argument("--skip-cis", action="store_true",
                        help="skip the focused CIS/ISO phase")
    parser.add_argument(
        "--stress-count",
        type=int,
        default=DEFAULT_STRESS_COUNT,
        help="bidirectional ACL stress iterations (default: %(default)s)",
    )
    parser.add_argument(
        "--skip-stress",
        action="store_true",
        help="skip long-running ACL/ISO stress (release result stays incomplete)",
    )
    args = parser.parse_args()

    if args.stress_count <= 0:
        parser.error("--stress-count must be positive")
    only_modes = sum((args.only_pawr, args.only_recovery,
                      args.only_bis_recovery))
    if only_modes > 1:
        parser.error(
            "--only-pawr, --only-recovery and --only-bis-recovery are mutually exclusive"
        )

    try:
        spec_a, spec_b = resolve_pair(args.a, args.b, kind=args.transport)
    except (HciError, SelectionError) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    print("Dongle A: %s" % spec_a)
    print("Dongle B: %s" % spec_b)

    book = ResultBook("HciController nRF52840 + SDC HEAD release validation")

    if args.only_pawr:
        with_pair(
            spec_a, spec_b, args.raw,
            run_pawr_phase,
            book, "A Advertiser / B Scanner",
        )
        book.print_report()
        return book.exit_code()

    if args.only_recovery:
        with_pair_establishment_retry(
            spec_a, spec_b, args.raw,
            run_recovery_phase,
            book, "A Advertiser / B Scanner",
        )
        book.print_report()
        return book.exit_code()

    if args.only_bis_recovery:
        run_iso_phase(
            bis_features,
            bis_features.run_bis_phase,
            book,
            "A Source / B Sink",
            spec_a,
            spec_b,
            raw=args.raw,
        )
        with_pair_establishment_retry(
            spec_a, spec_b, args.raw,
            run_recovery_phase,
            book, "A Advertiser / B Scanner",
        )
        book.print_report()
        return book.exit_code()

    run_profile_phase(book, spec_a, spec_b, args.raw)

    if args.skip_probe:
        book.incomplete("Broad command probe", "Dongle A", "skipped by command line")
        book.incomplete("Broad command probe", "Dongle B", "skipped by command line")
    else:
        run_probe(book, "Dongle A", spec_a, args.raw)
        run_probe(book, "Dongle B", spec_b, args.raw)

    with_pair_establishment_retry(
        spec_a, spec_b, args.raw,
        run_connected_feature_phase,
        book, "A Central -> B Peripheral",
    )
    with_pair_establishment_retry(
        spec_b, spec_a, args.raw,
        run_connected_feature_phase,
        book, "B Central -> A Peripheral",
    )

    with_pair_establishment_retry(
        spec_a, spec_b, args.raw,
        run_subrate_phase,
        book, "A Central -> B Peripheral",
    )
    with_pair_establishment_retry(
        spec_b, spec_a, args.raw,
        run_subrate_phase,
        book, "B Central -> A Peripheral",
    )

    with_pair_establishment_retry(
        spec_a, spec_b, args.raw,
        run_core_advanced_phase,
        book, "A Central / B Peripheral",
    )

    with_pair(
        spec_a, spec_b, args.raw,
        run_periodic_sync_phase,
        book, "A Advertiser / B Scanner",
    )
    with_pair_establishment_retry(
        spec_a, spec_b, args.raw,
        run_past_phase,
        book, "A Sender / B Receiver",
    )
    with_pair(
        spec_a, spec_b, args.raw,
        run_pawr_phase,
        book, "A Advertiser / B Scanner",
    )

    if args.skip_cis:
        book.incomplete("ISO", "CIS Central/Peripheral + bidirectional HCI ISO",
                        "skipped by command line")
    else:
        run_cis(book, spec_a, spec_b, args.raw)

    run_iso_phase(
        bis_features,
        bis_features.run_bis_phase,
        book,
        "A Source / B Sink",
        spec_a,
        spec_b,
        raw=args.raw,
    )

    with_pair_establishment_retry(
        spec_a, spec_b, args.raw,
        run_recovery_phase,
        book, "A Advertiser / B Scanner",
    )

    if args.skip_stress:
        book.incomplete(
            "Stress",
            "concurrent ACL/ISO/event traffic",
            "skipped by command line",
        )
    else:
        # A raw dump of 10k stress iterations is not useful as a default
        # release artifact. The focused phases above still honor --raw; stress
        # reports progress and the exact failing iteration if anything breaks.
        run_iso_phase(
            stress_features,
            stress_features.run_stress_phase,
            book,
            "A Central / B Peripheral",
            spec_a,
            spec_b,
            raw=False,
            count=args.stress_count,
        )

    book.print_report()
    return book.exit_code()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Two-HciController release acceptance harness.

Every advertised Release-1 capability is exercised positively. A procedure
that cannot be completed is FAIL, not a hidden skip.
"""

import argparse
from pathlib import Path
import subprocess
import sys

_HARNESS_DIR = Path(__file__).resolve().parents[1]
_TESTS_DIR = _HARNESS_DIR.parent
_HARDWARE_DIR = _TESTS_DIR / "hardware"
_REPO_ROOT = _TESTS_DIR.parent
if str(_HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(_HARNESS_DIR))
if str(_HARDWARE_DIR) not in sys.path:
    sys.path.insert(0, str(_HARDWARE_DIR))
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib.bis_features import run_bis_phase
from lib.connected_features_pair import run_connected_feature_phase, run_subrate_phase
from lib.core_advanced import run_core_advanced_phase
from lib.hci_pair import Hci, HciError, resolve_pair_ports
from lib.periodic_features import run_past_phase, run_pawr_phase, run_periodic_sync_phase
from lib.profile import read_controller_capabilities
from lib.recovery_features import run_recovery_phase
from lib.results import ResultBook
from lib.stress_features import DEFAULT_STRESS_COUNT, run_stress_phase
from pair_smoke_test import check_profile


def run_profile_phase(book, port_a, port_b, raw):
    a = Hci(port_a, raw=raw)
    b = Hci(port_b, raw=raw)
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


def with_pair(port_a, port_b, raw, callback, *args):
    a = Hci(port_a, raw=raw)
    b = Hci(port_b, raw=raw)
    try:
        callback(*args, a, b)
    finally:
        a.close()
        b.close()


def run_probe(book, label, port, raw):
    script = _TESTS_DIR / "hardware" / "hci_ble_test.py"
    command = [sys.executable, str(script), "--port", port]
    if raw:
        command.append("--raw")
    command += ["probe", "--consent", "--verbose"]
    result = subprocess.run(command, cwd=str(_REPO_ROOT))
    if result.returncode == 0:
        book.passed("Broad command probe", label)
    else:
        book.failed("Broad command probe", label,
                    "exit %d" % result.returncode)


def run_cis(book, port_a, port_b, raw):
    script = Path(__file__).resolve().parent / "cis_pair_test.py"
    command = [
        sys.executable,
        str(script),
        "--central", port_a,
        "--peripheral", port_b,
    ]
    if raw:
        command.append("--raw")
    result = subprocess.run(command, cwd=str(_REPO_ROOT))
    if result.returncode == 0:
        book.passed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO")
    else:
        book.failed("ISO", "CIS Central/Peripheral + bidirectional HCI ISO",
                    "exit %d" % result.returncode)


def main():
    parser = argparse.ArgumentParser(
        description="Run release-strict validation with two HciController dongles"
    )
    parser.add_argument("--a", help="first HciController H:4 serial port")
    parser.add_argument("--b", help="second HciController H:4 serial port")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    parser.add_argument("--only-pawr", action="store_true",
                        help="run only the PAwR advertiser/scanner phase")
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

    try:
        port_a, port_b = resolve_pair_ports(args.a, args.b)
    except HciError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    print("Dongle A: %s" % port_a)
    print("Dongle B: %s" % port_b)

    book = ResultBook("HciController nRF52840 + SDC HEAD release validation")

    if args.only_pawr:
        with_pair(
            port_a, port_b, args.raw,
            run_pawr_phase,
            book, "A Advertiser / B Scanner",
        )
        book.print_report()
        return book.exit_code()

    run_profile_phase(book, port_a, port_b, args.raw)

    if args.skip_probe:
        book.incomplete("Broad command probe", "Dongle A", "skipped by command line")
        book.incomplete("Broad command probe", "Dongle B", "skipped by command line")
    else:
        run_probe(book, "Dongle A", port_a, args.raw)
        run_probe(book, "Dongle B", port_b, args.raw)

    with_pair(
        port_a, port_b, args.raw,
        run_connected_feature_phase,
        book, "A Central -> B Peripheral",
    )
    with_pair(
        port_b, port_a, args.raw,
        run_connected_feature_phase,
        book, "B Central -> A Peripheral",
    )

    with_pair(
        port_a, port_b, args.raw,
        run_subrate_phase,
        book, "A Central -> B Peripheral",
    )
    with_pair(
        port_b, port_a, args.raw,
        run_subrate_phase,
        book, "B Central -> A Peripheral",
    )

    with_pair(
        port_a, port_b, args.raw,
        run_core_advanced_phase,
        book, "A Central / B Peripheral",
    )

    with_pair(
        port_a, port_b, args.raw,
        run_periodic_sync_phase,
        book, "A Advertiser / B Scanner",
    )
    with_pair(
        port_a, port_b, args.raw,
        run_past_phase,
        book, "A Sender / B Receiver",
    )
    with_pair(
        port_a, port_b, args.raw,
        run_pawr_phase,
        book, "A Advertiser / B Scanner",
    )

    if args.skip_cis:
        book.incomplete("ISO", "CIS Central/Peripheral + bidirectional HCI ISO",
                        "skipped by command line")
    else:
        run_cis(book, port_a, port_b, args.raw)

    run_bis_phase(
        book, "A Source / B Sink", port_a, port_b, raw=args.raw
    )

    with_pair(
        port_a, port_b, args.raw,
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
        run_stress_phase(
            book,
            "A Central / B Peripheral",
            port_a,
            port_b,
            raw=False,
            count=args.stress_count,
        )

    book.print_report()
    return book.exit_code()


if __name__ == "__main__":
    sys.exit(main())

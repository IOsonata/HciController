#!/usr/bin/env python3
"""A/B PAwR hardware check using the pre-reorg Host scheduling model."""

import argparse
import struct
import threading

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
from hcicontroller.hci_pair import Hci, HciError
from hcicontroller.hci_transport import SelectionError
from hcicontroller.pair_transport import resolve_pair
from hcicontroller.results import ResultBook


def _set_requested_pawr_subevent_data(hci, start, count):
    """Match the validated pre-reorg PAwR Host behavior exactly."""
    subevent = start
    marker = pf.PAWR_SUBEVENT_MARKER
    element = bytes([subevent, 0, 1, len(marker)]) + marker
    payload = bytes([pf.ADV_HANDLE_PERIODIC, 1]) + element
    pf._command_ok(
        hci,
        pf.OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA,
        payload,
        "LE Set Periodic Advertising Subevent Data",
    )
    return subevent


def _start_pawr_data_service(hci):
    service = {
        "stop": threading.Event(),
        "deferred": [],
        "errors": [],
        "finished": False,
    }

    def worker():
        try:
            while not service["stop"].is_set():
                packet = hci.read_packet(0.1)
                if packet is None:
                    continue
                kind, code, body = packet
                if (kind == pf.H4_EVENT and code == pf.EVT_LE_META
                        and len(body) >= 4
                        and body[0] == pf.LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST
                        and body[1] == pf.ADV_HANDLE_PERIODIC
                        and body[3] > 0):
                    _set_requested_pawr_subevent_data(
                        hci, body[2], body[3]
                    )
                    continue
                service["deferred"].append(packet)
        except (pf.HciError, pf.HciGone) as err:
            service["errors"].append(err)
            service["stop"].set()

    service["thread"] = threading.Thread(
        target=worker,
        name="pawr-data-service",
        daemon=True,
    )
    service["thread"].start()
    return service


def _finish_pawr_data_service(hci, service):
    if service is None or service["finished"]:
        return

    service["stop"].set()
    service["thread"].join(3.5)
    if service["thread"].is_alive():
        raise pf.HciError("PAwR data service did not stop")

    pf._base._restore(hci, service["deferred"])
    service["deferred"].clear()
    service["finished"] = True

    if service["errors"]:
        raise service["errors"][0]


def run_pre_reorg_pawr(book, label, advertiser, scanner):
    """Run the known-good pre-reorg PAwR Host sequence on current transports."""
    sync_handle = None
    data_service = None
    pawr_diag_before = None

    try:
        adv_id, adv_type, _ = pf.prepare_controller(advertiser)
        pf.prepare_controller(scanner)
        pawr_diag_before = pf._pawr_diag_counters(scanner)
        pf._configure_pawr(advertiser, adv_type)

        pf._ext_adv_enable(advertiser, pf.ADV_HANDLE_PERIODIC, True)
        pf._command_ok(
            advertiser,
            pf.OP_LE_SET_PERIODIC_ADV_ENABLE,
            bytes([1, pf.ADV_HANDLE_PERIODIC]),
            "LE Set Periodic Advertising Enable",
        )

        start, count = pf._wait_pawr_data_request(advertiser)
        subevent = _set_requested_pawr_subevent_data(
            advertiser, start, count
        )

        # This is the behavior that was removed by the Python reorganization:
        # one worker owns the advertiser while the main thread owns the scanner.
        data_service = _start_pawr_data_service(advertiser)

        # Use the current generic sync helper so the later exact AdvA/SID
        # cross-bench identity fix remains active during this A/B test.
        pf._create_periodic_sync(scanner, adv_id, adv_type)
        sync_handle, _ = pf._wait_sync_established(
            scanner,
            timeout=12.0,
            require_v2=True,
            keep_scan=True,
        )

        # Match the old 0x2084 behavior: do not purge reports that Hci.command()
        # deferred while waiting for Command Complete.
        payload = (
            struct.pack("<HHB", sync_handle, 0, 1)
            + bytes([subevent])
        )
        pf._command_ok(
            scanner,
            pf.OP_LE_SET_PERIODIC_SYNC_SUBEVENT,
            payload,
            "LE Set Periodic Sync Subevent",
        )

        report = pf._wait_periodic_report(
            scanner,
            sync_handle,
            pf.PAWR_SUBEVENT_MARKER,
            timeout=12.0,
            require_v2=True,
        )
        if report[1] is None or report[2] is None:
            raise pf.HciError(
                "PAwR periodic report had no event counter/subevent"
            )

        pf._set_pawr_response(
            scanner,
            sync_handle,
            report[1],
            report[2],
            subevent,
        )

        _finish_pawr_data_service(advertiser, data_service)
        data_service = None
        pf._set_sync_scan(scanner, False, allow_fail=True)

        pawr_diag_after = pf._pawr_diag_counters(scanner)
        pf._verify_pawr_response_completion(
            scanner,
            pawr_diag_before,
            pawr_diag_after,
        )

        pf._wait_pawr_response_report(
            advertiser,
            pf.PAWR_RESPONSE_MARKER,
        )

        book.passed(
            "PAwR pre-reorg A/B",
            "advertiser and scanner",
            "%s subevent %u + OTA response slot" % (label, subevent),
        )

    except (pf.HciError, pf.HciGone) as err:
        book.failed(
            "PAwR pre-reorg A/B",
            "advertiser and scanner",
            str(err),
        )

    finally:
        if data_service is not None:
            try:
                _finish_pawr_data_service(advertiser, data_service)
            except (pf.HciError, pf.HciGone):
                pass
        pf._set_sync_scan(scanner, False, allow_fail=True)
        if sync_handle is not None:
            pf._terminate_sync(scanner, sync_handle)
        pf._stop_periodic(advertiser)


def main():
    parser = argparse.ArgumentParser(
        description="Run PAwR with the validated pre-reorg Host scheduling model"
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "serial", "usb"),
        default="auto",
    )
    parser.add_argument("--a")
    parser.add_argument("--b")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    try:
        spec_a, spec_b = resolve_pair(
            args.a,
            args.b,
            kind=args.transport,
        )
    except (HciError, SelectionError) as err:
        print("FAIL: %s" % err)
        return 2

    print("Dongle A: %s" % spec_a)
    print("Dongle B: %s" % spec_b)

    book = ResultBook("PAwR pre-reorg Host A/B validation")
    advertiser = Hci(spec_a, raw=args.raw)
    scanner = Hci(spec_b, raw=args.raw)
    try:
        run_pre_reorg_pawr(
            book,
            "A Advertiser / B Scanner",
            advertiser,
            scanner,
        )
    finally:
        advertiser.close()
        scanner.close()

    book.print_report()
    return book.exit_code()


if __name__ == "__main__":
    raise SystemExit(main())

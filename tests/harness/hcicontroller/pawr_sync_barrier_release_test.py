#!/usr/bin/env python3
"""Run production PAwR with a strict 0x2084 synchronization barrier."""

import struct
import time

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
import release_test


_selected_subevent = {}
_original_wait_periodic_report = pf._wait_periodic_report


def _discard_prebarrier_reports(hci, sync_handle):
    """Drop same-sync periodic reports queued before 0x2084 completed."""
    kept = []
    discarded = []
    for packet in hci.pending:
        kind, code, body = packet
        if kind == pf.H4_EVENT and code == pf.EVT_LE_META:
            parsed = pf._periodic_report_data(body)
            if (
                    parsed is not None
                    and parsed[0] == sync_handle
                    and body[0] in (
                        pf.LE_PERIODIC_ADV_REPORT,
                        pf.LE_PERIODIC_ADV_REPORT_V2,
                    )):
                discarded.append(parsed)
                continue
        kept.append(packet)
    hci.pending = kept
    return discarded


def _set_pawr_sync_subevent_barrier(hci, sync_handle, subevent):
    payload = struct.pack("<HHB", sync_handle, 0, 1) + bytes([subevent])
    pf._command_ok(
        hci,
        pf.OP_LE_SET_PERIODIC_SYNC_SUBEVENT,
        payload,
        "LE Set Periodic Sync Subevent",
    )

    # Hci.command() cannot reach this Command Complete until it has consumed
    # every earlier packet on the ordered HCI stream. Reports it restored to
    # pending therefore belong to the pre-completion side of this barrier.
    discarded = _discard_prebarrier_reports(hci, sync_handle)
    _selected_subevent[sync_handle] = subevent
    if discarded:
        detail = ", ".join(
            "event=%s subevent=%s"
            % (
                "-" if report[1] is None else str(report[1]),
                "-" if report[2] is None else str(report[2]),
            )
            for report in discarded
        )
        print("PAWR-SYNC: discarded pre-0x2084-complete report(s): %s" % detail)


def _wait_periodic_report_selected(
        hci, sync_handle, marker, timeout=8.0, require_v2=False):
    selected = _selected_subevent.get(sync_handle)
    if selected is None:
        return _original_wait_periodic_report(
            hci,
            sync_handle,
            marker,
            timeout=timeout,
            require_v2=require_v2,
        )

    deferred = []
    wrong_subevents = []
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        packet = hci.read_packet(min(0.2, remaining))
        if packet is None:
            continue

        kind, code, body = packet
        if kind == pf.H4_EVENT and code == pf.EVT_LE_META:
            parsed = pf._periodic_report_data(body)
            if parsed is not None and parsed[0] == sync_handle:
                if require_v2 and body[0] != pf.LE_PERIODIC_ADV_REPORT_V2:
                    continue

                if body[0] == pf.LE_PERIODIC_ADV_REPORT_V2:
                    if parsed[2] != selected:
                        wrong_subevents.append((parsed[1], parsed[2]))
                        continue

                if marker in parsed[3]:
                    pf._base._restore(hci, deferred)
                    if wrong_subevents:
                        print(
                            "PAWR-SYNC: ignored post-barrier wrong-subevent "
                            "report(s): %s"
                            % ", ".join(
                                "event=%u subevent=%u" % item
                                for item in wrong_subevents
                            )
                        )
                    return parsed

        deferred.append(packet)

    pf._base._restore(hci, deferred)
    raise pf.HciError(
        "timed out waiting for post-0x2084 selected-subevent periodic marker"
    )


def main():
    pf._set_pawr_sync_subevent = _set_pawr_sync_subevent_barrier
    pf._wait_periodic_report = _wait_periodic_report_selected
    return release_test.main()


if __name__ == "__main__":
    raise SystemExit(main())

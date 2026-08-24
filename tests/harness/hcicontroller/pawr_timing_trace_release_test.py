#!/usr/bin/env python3
"""Run production PAwR with scanner/advertiser timing diagnostics."""

import struct
import time

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
import release_test


_state = {}
_original_set_sync_subevent = pf._set_pawr_sync_subevent
_original_wait_pawr_report = pf._wait_pawr_periodic_report
_original_set_response = pf._set_pawr_response


def _pending_summary(hci):
    counts = {}
    for packet in hci.pending:
        kind, code, body = packet
        if kind == pf.H4_EVENT and code == pf.EVT_LE_META and body:
            key = "LE-%02X" % body[0]
        else:
            key = "%02X/%02X" % (kind, 0xFF if code is None else code)
        counts[key] = counts.get(key, 0) + 1
    if not counts:
        return "none"
    return ",".join("%s:%u" % item for item in sorted(counts.items()))


def _set_pawr_sync_subevent_trace(hci, sync_handle, subevent):
    before = time.monotonic()
    result = _original_set_sync_subevent(hci, sync_handle, subevent)
    after = time.monotonic()
    _state.clear()
    _state.update({
        "sync_handle": sync_handle,
        "selected_subevent": subevent,
        "sync_complete": after,
    })
    print(
        "PAWR-TIME: 0x2084 complete subevent=%u command_ms=%.3f pending=%s"
        % (subevent, (after - before) * 1000.0, _pending_summary(hci))
    )
    return result


def _wait_pawr_periodic_report_trace(
        hci, sync_handle, subevent, marker, timeout=8.0):
    started = time.monotonic()
    report = _original_wait_pawr_report(
        hci, sync_handle, subevent, marker, timeout=timeout
    )
    received = time.monotonic()
    _state["report_time"] = received
    _state["request_event"] = report[1]
    _state["request_subevent"] = report[2]
    since_sync = None
    if _state.get("sync_complete") is not None:
        since_sync = (received - _state["sync_complete"]) * 1000.0
    print(
        "PAWR-TIME: selected 0x25 event=%u subevent=%u wait_ms=%.3f "
        "since_0x2084_ms=%s pending=%s"
        % (
            report[1],
            report[2],
            (received - started) * 1000.0,
            "-" if since_sync is None else "%.3f" % since_sync,
            _pending_summary(hci),
        )
    )
    return report


def _set_pawr_response_trace(
        hci, sync_handle, event_counter, request_subevent, response_subevent):
    started = time.monotonic()
    report_time = _state.get("report_time")
    turnaround = None if report_time is None else (started - report_time) * 1000.0
    print(
        "PAWR-TIME: 0x2083 send event=%u request_subevent=%u "
        "response_subevent=%u host_turnaround_ms=%s pending=%s"
        % (
            event_counter,
            request_subevent,
            response_subevent,
            "-" if turnaround is None else "%.3f" % turnaround,
            _pending_summary(hci),
        )
    )
    result = _original_set_response(
        hci,
        sync_handle,
        event_counter,
        request_subevent,
        response_subevent,
    )
    completed = time.monotonic()
    _state["response_complete"] = completed
    print(
        "PAWR-TIME: 0x2083 complete command_ms=%.3f"
        % ((completed - started) * 1000.0)
    )
    return result


def _response_report_summary(body, marker):
    subevent = body[2] if len(body) > 2 else -1
    tx_status = body[3] if len(body) > 3 else -1
    num_responses = body[4] if len(body) > 4 else 0
    at = 5
    entries = []
    marker_found = False

    for _ in range(num_responses):
        if len(body) < at + 6:
            entries.append("truncated-header")
            break
        tx_power = struct.unpack("<b", body[at:at + 1])[0]
        rssi = struct.unpack("<b", body[at + 1:at + 2])[0]
        cte_type = body[at + 2]
        response_slot = body[at + 3]
        data_status = body[at + 4]
        data_len = body[at + 5]
        if len(body) < at + 6 + data_len:
            entries.append("slot=%u truncated-data" % response_slot)
            break
        data = body[at + 6:at + 6 + data_len]
        found = marker in data
        marker_found = marker_found or found
        entries.append(
            "slot=%u status=%u len=%u marker=%s tx=%d rssi=%d cte=%u"
            % (
                response_slot,
                data_status,
                data_len,
                "yes" if found else "no",
                tx_power,
                rssi,
                cte_type,
            )
        )
        at += 6 + data_len

    return subevent, tx_status, num_responses, entries, marker_found


def _wait_pawr_response_report_trace(hci, marker, timeout=10.0):
    deferred = []
    deadline = time.monotonic() + timeout
    seen = []

    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        packet = hci.read_packet(min(0.2, remaining))
        if packet is None:
            continue

        kind, code, body = packet
        if (kind == pf.H4_EVENT and code == pf.EVT_LE_META
                and len(body) >= 5
                and body[0] == pf.LE_PERIODIC_ADV_RESPONSE_REPORT):
            if body[1] != pf.ADV_HANDLE_PERIODIC:
                deferred.append(packet)
                continue

            summary = _response_report_summary(body, marker)
            subevent, tx_status, num_responses, entries, marker_found = summary
            since_response = None
            if _state.get("response_complete") is not None:
                since_response = (
                    time.monotonic() - _state["response_complete"]
                ) * 1000.0
            text = (
                "subevent=%u tx_status=%u responses=%u after_0x2083_ms=%s %s"
                % (
                    subevent,
                    tx_status,
                    num_responses,
                    "-" if since_response is None else "%.3f" % since_response,
                    "; ".join(entries) if entries else "no-response-elements",
                )
            )
            seen.append(text)
            print("PAWR-TIME: advertiser 0x28 " + text)
            if marker_found:
                pf._base._restore(hci, deferred)
                return

        deferred.append(packet)

    pf._base._restore(hci, deferred)
    detail = " | ".join(seen) if seen else "no advertiser 0x28 events"
    raise pf.HciError(
        "no PAwR response marker; timing trace: %s" % detail
    )


def main():
    pf._set_pawr_sync_subevent = _set_pawr_sync_subevent_trace
    pf._wait_pawr_periodic_report = _wait_pawr_periodic_report_trace
    pf._set_pawr_response = _set_pawr_response_trace
    pf._wait_pawr_response_report = _wait_pawr_response_report_trace
    return release_test.main()


if __name__ == "__main__":
    raise SystemExit(main())

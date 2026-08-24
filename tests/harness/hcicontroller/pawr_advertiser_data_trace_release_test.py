#!/usr/bin/env python3
"""Run production PAwR while recording advertiser 0x27/0x2082 timing."""

import threading
import time

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
import release_test


_trace = {
    "origin": time.monotonic(),
    "requests": [],
    "response_send": None,
    "response_complete": None,
    "finish_calls": [],
    "response_reports": [],
}

_original_set_data = pf._set_requested_pawr_subevent_data
_original_start_service = pf._start_pawr_data_service
_original_finish_service = pf._finish_pawr_data_service
_original_set_response = pf._set_pawr_response
_original_wait_response = pf._wait_pawr_response_report


def _now_ms():
    return (time.monotonic() - _trace["origin"]) * 1000.0


def _set_requested_pawr_subevent_data_trace(hci, start, count):
    item = {
        "thread": threading.current_thread().name,
        "start": start,
        "count": count,
        "begin_ms": _now_ms(),
        "end_ms": None,
        "status": "pending",
    }
    _trace["requests"].append(item)
    try:
        result = _original_set_data(hci, start, count)
        item["status"] = "ok"
        return result
    except Exception as err:
        item["status"] = "%s: %s" % (type(err).__name__, err)
        raise
    finally:
        item["end_ms"] = _now_ms()


def _set_pawr_response_trace(
        hci, sync_handle, event_counter, request_subevent, response_subevent):
    _trace["response_send"] = {
        "time_ms": _now_ms(),
        "event": event_counter,
        "request_subevent": request_subevent,
        "response_subevent": response_subevent,
    }
    try:
        return _original_set_response(
            hci,
            sync_handle,
            event_counter,
            request_subevent,
            response_subevent,
        )
    finally:
        _trace["response_complete"] = _now_ms()


def _finish_pawr_data_service_trace(hci, service):
    entry = {
        "begin_ms": _now_ms(),
        "deferred_before": len(service["deferred"]) if service is not None else -1,
        "errors_before": len(service["errors"]) if service is not None else -1,
        "end_ms": None,
    }
    _trace["finish_calls"].append(entry)
    try:
        return _original_finish_service(hci, service)
    finally:
        entry["end_ms"] = _now_ms()


def _wait_pawr_response_report_trace(hci, marker, timeout=10.0):
    deferred = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        packet = hci.read_packet(min(0.2, remaining))
        if packet is None:
            continue
        kind, code, body = packet
        if (kind == pf.H4_EVENT and code == pf.EVT_LE_META
                and len(body) >= 5
                and body[0] == pf.LE_PERIODIC_ADV_RESPONSE_REPORT):
            report = {
                "time_ms": _now_ms(),
                "handle": body[1],
                "subevent": body[2],
                "tx_status": body[3],
                "responses": body[4],
                "marker": False,
            }
            at = 5
            for _ in range(body[4]):
                if len(body) < at + 6:
                    break
                data_len = body[at + 5]
                if len(body) < at + 6 + data_len:
                    break
                data = body[at + 6:at + 6 + data_len]
                if marker in data:
                    report["marker"] = True
                at += 6 + data_len
            _trace["response_reports"].append(report)
            if body[1] == pf.ADV_HANDLE_PERIODIC and report["marker"]:
                pf._base._restore(hci, deferred)
                return
        deferred.append(packet)

    pf._base._restore(hci, deferred)
    raise pf.HciError("no PAwR response report containing the response marker")


def _dump_trace(failed):
    requests = _trace["requests"]
    response_send = _trace["response_send"]
    response_complete = _trace["response_complete"]
    reports = _trace["response_reports"]

    print(
        "PAWR-ADVTRACE: result=%s requests=%u response_reports=%u"
        % ("FAIL" if failed else "PASS", len(requests), len(reports))
    )
    if response_send is not None:
        print(
            "PAWR-ADVTRACE: 0x2083 event=%u req_sub=%u rsp_sub=%u "
            "send_ms=%.3f complete_ms=%s"
            % (
                response_send["event"],
                response_send["request_subevent"],
                response_send["response_subevent"],
                response_send["time_ms"],
                "-" if response_complete is None else "%.3f" % response_complete,
            )
        )

    tail = requests[-12:] if failed else requests[-4:]
    for index, item in enumerate(tail, start=max(1, len(requests) - len(tail) + 1)):
        duration = None
        if item["end_ms"] is not None:
            duration = item["end_ms"] - item["begin_ms"]
        relative = None
        if response_send is not None:
            relative = item["begin_ms"] - response_send["time_ms"]
        print(
            "PAWR-ADVTRACE: 0x2082 #%u thread=%s start=%u count=%u "
            "begin_ms=%.3f rel_0x2083_ms=%s duration_ms=%s status=%s"
            % (
                index,
                item["thread"],
                item["start"],
                item["count"],
                item["begin_ms"],
                "-" if relative is None else "%.3f" % relative,
                "-" if duration is None else "%.3f" % duration,
                item["status"],
            )
        )

    for entry in _trace["finish_calls"]:
        print(
            "PAWR-ADVTRACE: finish_service begin_ms=%.3f end_ms=%s "
            "deferred_before=%d errors_before=%d"
            % (
                entry["begin_ms"],
                "-" if entry["end_ms"] is None else "%.3f" % entry["end_ms"],
                entry["deferred_before"],
                entry["errors_before"],
            )
        )

    for report in reports:
        print(
            "PAWR-ADVTRACE: 0x28 time_ms=%.3f subevent=%u tx_status=%u "
            "responses=%u marker=%s"
            % (
                report["time_ms"],
                report["subevent"],
                report["tx_status"],
                report["responses"],
                "yes" if report["marker"] else "no",
            )
        )


def main():
    pf._set_requested_pawr_subevent_data = _set_requested_pawr_subevent_data_trace
    pf._set_pawr_response = _set_pawr_response_trace
    pf._finish_pawr_data_service = _finish_pawr_data_service_trace
    pf._wait_pawr_response_report = _wait_pawr_response_report_trace

    rc = release_test.main()
    _dump_trace(rc != 0)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

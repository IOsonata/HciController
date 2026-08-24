#!/usr/bin/env python3
"""Run production PAwR with continuous advertiser HCI ownership."""

import threading
import time

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
import release_test


_active = {"service": None}
_original_finish = pf._finish_pawr_data_service
_original_set_response = pf._set_pawr_response
_original_wait_response = pf._wait_pawr_response_report


def _response_has_marker(body, marker):
    if len(body) < 5 or body[0] != pf.LE_PERIODIC_ADV_RESPONSE_REPORT:
        return False
    if body[1] != pf.ADV_HANDLE_PERIODIC:
        return False

    at = 5
    for _ in range(body[4]):
        if len(body) < at + 6:
            return False
        data_len = body[at + 5]
        if len(body) < at + 6 + data_len:
            return False
        if marker in body[at + 6:at + 6 + data_len]:
            return True
        at += 6 + data_len
    return False


def _start_pawr_data_service_owner(hci):
    service = {
        "stop": threading.Event(),
        "response": threading.Event(),
        "deferred": [],
        "errors": [],
        "finished": False,
        "armed": False,
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
                    pf._set_requested_pawr_subevent_data(
                        hci, body[2], body[3]
                    )
                    continue
                if (kind == pf.H4_EVENT and code == pf.EVT_LE_META
                        and _response_has_marker(body, pf.PAWR_RESPONSE_MARKER)):
                    service["response"].set()
                    continue
                service["deferred"].append(packet)
        except (pf.HciError, pf.HciGone) as err:
            service["errors"].append(err)
            service["stop"].set()

    service["thread"] = threading.Thread(
        target=worker,
        name="pawr-advertiser-owner",
        daemon=True,
    )
    service["thread"].start()
    _active["service"] = service
    return service


def _set_pawr_response_owner(
        hci, sync_handle, event_counter, request_subevent, response_subevent):
    result = _original_set_response(
        hci,
        sync_handle,
        event_counter,
        request_subevent,
        response_subevent,
    )
    service = _active.get("service")
    if service is not None:
        service["armed"] = True
    return result


def _finish_pawr_data_service_owner(hci, service):
    # Production currently hands advertiser HCI ownership back to the main
    # thread immediately after 0x2083 completes. Keep the worker alive instead
    # when the response is still in flight; it remains the sole advertiser
    # reader and continues servicing 0x27 while waiting for 0x28.
    if (service is _active.get("service")
            and service.get("armed")
            and not service["response"].is_set()):
        return
    return _original_finish(hci, service)


def _wait_pawr_response_report_owner(hci, marker, timeout=10.0):
    service = _active.get("service")
    if service is None or marker != pf.PAWR_RESPONSE_MARKER:
        return _original_wait_response(hci, marker, timeout=timeout)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if service["response"].wait(0.05):
            break
        if service["errors"]:
            break

    try:
        if service["errors"]:
            raise service["errors"][0]
        if not service["response"].is_set():
            raise pf.HciError(
                "no PAwR response marker observed by continuous advertiser reader"
            )
    finally:
        if not service["finished"]:
            _original_finish(hci, service)
        if _active.get("service") is service:
            _active["service"] = None


def main():
    pf._start_pawr_data_service = _start_pawr_data_service_owner
    pf._set_pawr_response = _set_pawr_response_owner
    pf._finish_pawr_data_service = _finish_pawr_data_service_owner
    pf._wait_pawr_response_report = _wait_pawr_response_report_owner
    return release_test.main()


if __name__ == "__main__":
    raise SystemExit(main())

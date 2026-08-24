#!/usr/bin/env python3
"""Run the production PAwR release path with pre-reorg one-subevent refill."""

import _bootstrap  # noqa: F401
from hcicontroller import periodic_features as pf
import release_test


def _set_requested_pawr_subevent_data(hci, start, count):
    """Supply only the requested start subevent, matching pre-reorg behavior."""
    if start >= pf.PAWR_NUM_SUBEVENTS:
        raise pf.HciError("PAwR data request start exceeds configured subevents")
    if count <= 0:
        raise pf.HciError("PAwR data request count is zero")

    marker = pf.PAWR_SUBEVENT_MARKER
    element = bytes([start, 0, 1, len(marker)]) + marker
    payload = bytes([pf.ADV_HANDLE_PERIODIC, 1]) + element
    pf._command_ok(
        hci,
        pf.OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA,
        payload,
        "LE Set Periodic Advertising Subevent Data",
    )
    return start


def main():
    pf._set_requested_pawr_subevent_data = _set_requested_pawr_subevent_data
    return release_test.main()


if __name__ == "__main__":
    raise SystemExit(main())

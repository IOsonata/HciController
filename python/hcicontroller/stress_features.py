#!/usr/bin/env python3
"""Long-running concurrent ACL/ISO/event traffic on one ACL+CIS pair."""

import struct
import time

from hci_cis_pair_test import (
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    H4_EVENT,
    H4_ISO,
    ISO_DIRECTION_INPUT,
    ISO_DIRECTION_OUTPUT,
    IsoHci,
    OP_DISCONNECT,
    OP_LE_ACCEPT_CIS_REQUEST,
    OP_LE_CREATE_CIS,
    OP_LE_REMOVE_CIG,
    OP_LE_REMOVE_ISO_DATA_PATH,
    cleanup,
    counter_delta,
    counter_snapshot,
    create_cig,
    prepare,
    read_iso_buffers,
    setup_iso_path,
    start_advertising,
    start_connection,
    wait_acl_pair,
    wait_cis_established,
    wait_cis_request,
    wait_iso,
)

from . import connected_features as _base
from .connected_features_pair import wait_acl_marker

HciError = _base.HciError
HciGone = _base.HciGone
status_text = _base.status_text

EVT_NUM_COMPLETED_PACKETS = 0x13
STRESS_CID = 0x0040
DEFAULT_STRESS_COUNT = 10000
ISO_EVERY = 10
ESTABLISHMENT_ATTEMPTS = 3
ESTABLISHMENT_REASON = 0x3E

# Any increment in these counters means the stress run crossed a boundary the
# release transport is supposed to preserve. Retries are deliberately not in
# this set; bounded retry is part of normal backpressure handling.
ERROR_COUNTERS = {
    5: "event backpressure",
    6: "ACL refused by controller",
    7: "ISO refused by controller",
    9: "controller queue errors",
    10: "unknown output types",
    11: "bad output lengths",
    13: "oversize ACL from host",
    14: "ISO dropped",
    15: "credit table overflow",
    18: "H4 bad packet indicator",
    19: "H4 oversize packet",
    21: "transport read errors",
    22: "transport write errors",
    24: "transport tx oversize",
    26: "host packet rejected",
    27: "controller get errors",
    28: "controller packet rejected",
    29: "controller packet unsendable",
    30: "host over its ACL credits",
    31: "link table overflow",
}


def _discard_pending_iso(hci):
    """Drop background CIS output that is not part of the current ISO marker."""
    if hci.pending:
        hci.pending = [
            packet for packet in hci.pending
            if packet[0] != H4_ISO
        ]


def _wait_stress_acl_marker(hci, expected_handle, expected_cid,
                            expected_payload, timeout=3.0):
    """Wait for one ACL marker without carrying CIS output into the next wait."""
    try:
        return wait_acl_marker(
            hci,
            expected_handle,
            expected_cid,
            expected_payload,
            timeout=timeout,
        )
    finally:
        _discard_pending_iso(hci)


def _wait_completed(hci, expected_handle, timeout=3.0):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_NUM_COMPLETED_PACKETS and body:
            count = body[0]
            for i in range(count):
                at = 1 + (4 * i)
                if len(body) < at + 4:
                    break
                handle, done = struct.unpack("<HH", body[at:at + 4])
                if (handle & 0x0FFF) == expected_handle and done:
                    _base._restore(hci, deferred)
                    return
            continue
        if kind == H4_ISO:
            continue
        deferred.append(packet)
    _base._restore(hci, deferred)
    raise HciError("no Number Of Completed Packets for handle 0x%04X"
                   % expected_handle)


def _check_counter_deltas(before, after, label):
    if before is None or after is None:
        raise HciError("%s HciController counters unavailable" % label)
    for index, name in ERROR_COUNTERS.items():
        delta = counter_delta(before, after, index)
        if delta not in (None, 0):
            raise HciError("%s counter %s increased by %d"
                           % (label, name, delta))


def _link_establishment_failed(hci):
    down = getattr(hci, "link_down", None)
    return (
        down is not None
        and len(down) >= 3
        and down[2] == ESTABLISHMENT_REASON
    )


def _retryable_acl_establishment(err, central, peripheral):
    detail = str(err)
    if (
        "ACL connection failed: 0x3E" in detail
        or "Connection Failed To Be Established" in detail
    ):
        return True
    return (
        "timed out waiting for both ACL connection events" in detail
        and (
            _link_establishment_failed(central)
            or _link_establishment_failed(peripheral)
        )
    )


def _cleanup_setup_attempt(central, peripheral):
    cleanup(central, peripheral, None, None, None, None, None)
    central.close()
    peripheral.close()


def _establish_acl_with_retry(central_port, peripheral_port, raw, label,
                              attempts=ESTABLISHMENT_ATTEMPTS):
    for attempt in range(1, attempts + 1):
        central = IsoHci(central_port, raw=raw)
        peripheral = IsoHci(peripheral_port, raw=raw)
        try:
            peripheral_id, peripheral_type, _ = prepare(peripheral)
            _, central_type, _ = prepare(central)
            start_advertising(peripheral, peripheral_id, peripheral_type)
            start_connection(
                central, central_type, peripheral_id, peripheral_type
            )
            central_acl, peripheral_acl = wait_acl_pair(central, peripheral)
        except (HciError, HciGone) as err:
            retryable = _retryable_acl_establishment(
                err, central, peripheral
            )
            _cleanup_setup_attempt(central, peripheral)
            if retryable and attempt < attempts:
                print(
                    "HOST-RETRY: Stress %s ACL setup attempt %u/%u ended "
                    "in LE 0x3E; retrying fresh setup"
                    % (label, attempt, attempts)
                )
                continue
            if retryable:
                print(
                    "HOST-RETRY: Stress %s ACL setup still failed with "
                    "LE 0x3E after %u attempts"
                    % (label, attempts)
                )
            raise

        if attempt > 1:
            print(
                "HOST-RETRY: Stress %s ACL setup recovered on attempt %u/%u"
                % (label, attempt, attempts)
            )
        return central, peripheral, central_acl, peripheral_acl

    raise HciError("unreachable stress ACL establishment retry state")


def run_stress_phase(book, label, central_port, peripheral_port,
                     raw=False, count=DEFAULT_STRESS_COUNT):
    central = None
    peripheral = None
    cig_id = None
    central_cis = None
    peripheral_cis = None
    central_acl = None
    peripheral_acl = None
    try:
        central, peripheral, central_acl, peripheral_acl = \
            _establish_acl_with_retry(
                central_port, peripheral_port, raw, label
            )

        cig_id, central_cis, cis_id = create_cig(central)
        status, _ = central.command(
            OP_LE_CREATE_CIS,
            bytes([1]) + struct.pack("<HH", central_cis, central_acl),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Create CIS returned %s" % status_text(status))

        peripheral_cis = wait_cis_request(
            peripheral, peripheral_acl, cig_id, cis_id
        )
        status, _ = peripheral.command(
            OP_LE_ACCEPT_CIS_REQUEST,
            struct.pack("<H", peripheral_cis),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Accept CIS Request returned %s"
                           % status_text(status))
        wait_cis_established(peripheral, "peripheral", peripheral_cis)
        wait_cis_established(central, "central", central_cis)

        for hci, cis in (
            (central, central_cis),
            (peripheral, peripheral_cis),
        ):
            setup_iso_path(hci, cis, ISO_DIRECTION_INPUT)
            setup_iso_path(hci, cis, ISO_DIRECTION_OUTPUT)
        read_iso_buffers(central, "stress central")
        read_iso_buffers(peripheral, "stress peripheral")

        central_before = counter_snapshot(central)
        peripheral_before = counter_snapshot(peripheral)

        for i in range(count):
            seq = struct.pack("<I", i)
            c_acl = b"C" + seq
            p_acl = b"P" + seq

            central.send_acl(central_acl, STRESS_CID, c_acl)
            if not _wait_stress_acl_marker(
                peripheral, peripheral_acl, STRESS_CID, c_acl
            ):
                raise HciError("ACL C->P marker %u missing" % i)
            _wait_completed(central, central_acl)

            peripheral.send_acl(peripheral_acl, STRESS_CID, p_acl)
            if not _wait_stress_acl_marker(
                central, central_acl, STRESS_CID, p_acl
            ):
                raise HciError("ACL P->C marker %u missing" % i)
            _wait_completed(peripheral, peripheral_acl)

            if (i % ISO_EVERY) == 0:
                c_iso = b"CI" + seq
                p_iso = b"PI" + seq
                central.send_iso(central_cis, i, c_iso)
                wait_iso(
                    peripheral, "stress peripheral", peripheral_cis,
                    c_iso, timeout=3.0,
                )
                _wait_completed(central, central_cis)

                peripheral.send_iso(peripheral_cis, i, p_iso)
                wait_iso(
                    central, "stress central", central_cis,
                    p_iso, timeout=3.0,
                )
                _wait_completed(peripheral, peripheral_cis)

            if i and (i % 1000) == 0:
                print("Stress progress: %d/%d iterations" % (i, count))

        central_after = counter_snapshot(central)
        peripheral_after = counter_snapshot(peripheral)
        _check_counter_deltas(central_before, central_after, "central")
        _check_counter_deltas(peripheral_before, peripheral_after, "peripheral")

        book.passed(
            "Stress",
            "concurrent ACL/ISO/event traffic",
            "%s %u bidirectional ACL iterations + %u bidirectional ISO iterations"
            % (label, count, (count + ISO_EVERY - 1) // ISO_EVERY),
        )
    except (HciError, HciGone) as err:
        book.failed("Stress", "concurrent ACL/ISO/event traffic", str(err))
    finally:
        if central is not None and peripheral is not None:
            cleanup(
                central,
                peripheral,
                cig_id,
                central_cis,
                peripheral_cis,
                central_acl,
                peripheral_acl,
            )
            central.close()
            peripheral.close()

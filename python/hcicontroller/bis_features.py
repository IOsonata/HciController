#!/usr/bin/env python3
"""Unencrypted BIG/BIS OTA validation using the shared HCI ISO helpers."""

import struct

from hci_cis_pair_test import (
    ISO_DIRECTION_INPUT,
    ISO_DIRECTION_OUTPUT,
    IsoHci,
    counter_delta,
    counter_snapshot,
    read_iso_buffers,
    setup_iso_path,
    wait_iso,
)

from . import connected_features as _base
from .hci_pair import prepare_controller
from .periodic_features import (
    ADV_HANDLE_PERIODIC,
    _configure_periodic,
    _create_periodic_sync,
    _stop_periodic,
    _start_periodic,
    _terminate_sync,
    _wait_periodic_report,
    _wait_sync_established,
    PERIODIC_MARKER,
)

HciError = _base.HciError
HciGone = _base.HciGone
status_text = _base.status_text

OP_LE_CREATE_BIG = 0x2068
OP_LE_TERMINATE_BIG = 0x206A
OP_LE_BIG_CREATE_SYNC = 0x206B
OP_LE_BIG_TERMINATE_SYNC = 0x206C
OP_LE_SETUP_ISO_DATA_PATH = 0x206E
OP_LE_REMOVE_ISO_DATA_PATH = 0x206F

LE_BIG_COMPLETE = 0x1B
LE_TERMINATE_BIG_COMPLETE = 0x1C
LE_BIG_SYNC_ESTABLISHED = 0x1D
LE_BIGINFO_ADV_REPORT = 0x22

SOURCE_BIG_HANDLE = 0
SINK_BIG_HANDLE = 1
BIS_INDEX = 1
BIG_TERMINATE_REASON = 0x16
BIS_MARKER = b"BIS source->sink"


def _wait_biginfo(hci, sync_handle, timeout=8.0):
    body = _base.wait_le_subevent(
        hci,
        LE_BIGINFO_ADV_REPORT,
        handle=sync_handle,
        handle_offset=1,
        predicate=lambda event: len(event) >= 4 and event[3] >= 1,
        timeout=timeout,
    )
    if body is None:
        raise HciError("no LE BIGInfo Advertising Report for periodic sync")


def _create_big(source):
    payload = bytes([
        SOURCE_BIG_HANDLE,
        ADV_HANDLE_PERIODIC,
        1,
    ])
    payload += bytes((0x10, 0x27, 0x00))  # SDU interval: 10000 us
    payload += struct.pack("<HH", 40, 20)
    payload += bytes([
        2,      # RTN
        1,      # LE 1M PHY
        0,      # sequential packing
        0,      # unframed
        0,      # unencrypted: Release-1 nRF52840 profile
    ])
    payload += bytes(16)
    if len(payload) != 31:
        raise AssertionError("LE Create BIG payload length")

    status, _ = source.command(OP_LE_CREATE_BIG, payload, allow_fail=True)
    if status != 0:
        raise HciError("LE Create BIG returned %s" % status_text(status))

    body = _base.wait_le_subevent(
        source,
        LE_BIG_COMPLETE,
        predicate=lambda event: (
            len(event) >= 21
            and event[1] == 0
            and event[2] == SOURCE_BIG_HANDLE
            and event[18] == 1
        ),
        timeout=10.0,
    )
    if body is None:
        raise HciError("no successful LE Create BIG Complete event")
    return struct.unpack("<H", body[19:21])[0] & 0x0FFF


def _create_big_sync(sink, periodic_sync):
    payload = bytes([SINK_BIG_HANDLE])
    payload += struct.pack("<H", periodic_sync)
    payload += bytes([0]) + bytes(16)  # unencrypted and zero Broadcast_Code
    payload += bytes([0])              # MSE: controller may schedule all subevents
    payload += struct.pack("<H", 0x0200)
    payload += bytes([1, BIS_INDEX])

    status, _ = sink.command(OP_LE_BIG_CREATE_SYNC, payload, allow_fail=True)
    if status != 0:
        raise HciError("LE BIG Create Sync returned %s" % status_text(status))

    body = _base.wait_le_subevent(
        sink,
        LE_BIG_SYNC_ESTABLISHED,
        predicate=lambda event: (
            len(event) >= 17
            and event[1] == 0
            and event[2] == SINK_BIG_HANDLE
            and event[14] == 1
        ),
        timeout=10.0,
    )
    if body is None:
        raise HciError("no successful LE BIG Sync Established event")
    return struct.unpack("<H", body[15:17])[0] & 0x0FFF


def _wait_big_terminated(source, timeout=8.0):
    body = _base.wait_le_subevent(
        source,
        LE_TERMINATE_BIG_COMPLETE,
        predicate=lambda event: (
            len(event) >= 3
            and event[1] == SOURCE_BIG_HANDLE
        ),
        timeout=timeout,
    )
    if body is None:
        raise HciError("no LE Terminate BIG Complete event")
    if body[2] != BIG_TERMINATE_REASON:
        raise HciError(
            "LE Terminate BIG Complete reason 0x%02X, expected 0x%02X"
            % (body[2], BIG_TERMINATE_REASON)
        )


def _terminate_big(source):
    status, _ = source.command(
        OP_LE_TERMINATE_BIG,
        bytes([SOURCE_BIG_HANDLE, BIG_TERMINATE_REASON]),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Terminate BIG returned %s" % status_text(status))
    _wait_big_terminated(source)


def _terminate_big_sync(sink):
    status, _ = sink.command(
        OP_LE_BIG_TERMINATE_SYNC,
        bytes([SINK_BIG_HANDLE]),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE BIG Terminate Sync returned %s" % status_text(status))


def _remove_iso_path(hci, handle, direction_mask):
    try:
        hci.command(
            OP_LE_REMOVE_ISO_DATA_PATH,
            struct.pack("<HB", handle, direction_mask),
            allow_fail=True,
        )
    except (HciError, HciGone):
        pass


def run_bis_phase(book, label, source_port, sink_port, raw=False):
    source = IsoHci(source_port, raw=raw)
    sink = IsoHci(sink_port, raw=raw)
    periodic_sync = None
    source_bis = None
    sink_bis = None
    big_created = False
    big_synced = False
    try:
        source_id, source_type, _ = prepare_controller(source)
        prepare_controller(sink)

        _configure_periodic(source, source_type)
        _start_periodic(source)
        _create_periodic_sync(sink, source_id, source_type)
        periodic_sync, _ = _wait_sync_established(sink)
        _wait_periodic_report(sink, periodic_sync, PERIODIC_MARKER)

        source_bis = _create_big(source)
        big_created = True
        _wait_biginfo(sink, periodic_sync)
        sink_bis = _create_big_sync(sink, periodic_sync)
        big_synced = True

        setup_iso_path(source, source_bis, ISO_DIRECTION_INPUT)
        setup_iso_path(sink, sink_bis, ISO_DIRECTION_OUTPUT)
        source_iso = read_iso_buffers(source, "BIS source")
        sink_iso = read_iso_buffers(sink, "BIS sink")
        if min(source_iso, sink_iso) < len(BIS_MARKER) + 4:
            raise HciError("HCI ISO buffer is too small for BIS marker")

        source_before = counter_snapshot(source)
        source.send_iso(source_bis, 0, BIS_MARKER)
        wait_iso(sink, "BIS sink", sink_bis, BIS_MARKER, timeout=8.0)
        source_after = counter_snapshot(source)

        source_taken = counter_delta(source_before, source_after, 17)
        source_drop = counter_delta(source_before, source_after, 14)
        if source_drop not in (None, 0):
            raise HciError("routing layer dropped BIS source ISO data")
        if source_taken is not None and source_taken < 1:
            raise HciError("BIS source ISO packet never reached SDC")

        # A successful data transfer is not the end of the BIS phase. The sink
        # termination is complete when LE BIG Terminate Sync completes; BIG Sync
        # Lost reports an unrequested loss and is not expected here. The source
        # termination is asynchronous, so wait for LE Terminate BIG Complete.
        _remove_iso_path(source, source_bis, 0x01)
        source_bis = None
        _remove_iso_path(sink, sink_bis, 0x02)
        sink_bis = None

        _terminate_big_sync(sink)
        big_synced = False
        _terminate_big(source)
        big_created = False

        _terminate_sync(sink, periodic_sync)
        periodic_sync = None

        # Before either Bulk-Serialization USB interface is closed or switched
        # back to legacy HCI, prove the former BIS sink can create a fresh
        # periodic synchronization to the still-running advertising train. A
        # failure here is controller/SDC state left by BIG-sink teardown, not a
        # USB alternate-setting transition.
        _create_periodic_sync(sink, source_id, source_type)
        periodic_sync, _ = _wait_sync_established(sink)
        _wait_periodic_report(sink, periodic_sync, PERIODIC_MARKER)
        _terminate_sync(sink, periodic_sync)
        periodic_sync = None
        _stop_periodic(source)

        book.passed(
            "ISO",
            "BIS Source -> Sink",
            "%s unencrypted BIG, OTA SDU + synchronous BIG teardown + sink resync"
            % label,
        )
    except (HciError, HciGone) as err:
        book.failed("ISO", "BIS Source -> Sink", str(err))
    finally:
        if source_bis is not None:
            _remove_iso_path(source, source_bis, 0x01)
        if sink_bis is not None:
            _remove_iso_path(sink, sink_bis, 0x02)
        if big_synced:
            try:
                sink.command(
                    OP_LE_BIG_TERMINATE_SYNC,
                    bytes([SINK_BIG_HANDLE]),
                    allow_fail=True,
                )
            except (HciError, HciGone):
                pass
        if big_created:
            try:
                source.command(
                    OP_LE_TERMINATE_BIG,
                    bytes([SOURCE_BIG_HANDLE, BIG_TERMINATE_REASON]),
                    allow_fail=True,
                )
            except (HciError, HciGone):
                pass
        if periodic_sync is not None:
            _terminate_sync(sink, periodic_sync)
        _stop_periodic(source)
        source.close()
        sink.close()

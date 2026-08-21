#!/usr/bin/env python3
"""Recovery checks after active periodic advertising/synchronization state."""

import struct
import time

from . import connected_features as _base
from .connected_features_pair import wait_acl_marker
from .hci_pair import (
    disconnect_acl_pair,
    prepare_controller,
    start_legacy_advertising,
    start_legacy_connection,
    wait_acl_pair,
)
from .periodic_features import (
    PERIODIC_MARKER,
    _configure_periodic,
    _create_periodic_sync,
    _start_periodic,
    _stop_periodic,
    _terminate_sync,
    _wait_periodic_report,
    _wait_sync_established,
)

HciError = _base.HciError
HciGone = _base.HciGone

LE_PERIODIC_SYNC_LOST = 0x10
RECOVERY_CID = 0x0040
RECOVERY_MARKER = b"HCI recovery ACL"


def _sync_once(advertiser, scanner, adv_id, adv_type):
    _create_periodic_sync(scanner, adv_id, adv_type)
    sync, _ = _wait_sync_established(scanner)
    _wait_periodic_report(scanner, sync, PERIODIC_MARKER)
    return sync


def _wait_sync_lost(scanner, sync_handle, timeout=7.0):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = scanner.read_packet(
            min(0.2, max(0.0, deadline - time.time()))
        )
        if packet is None:
            continue
        kind, code, body = packet
        if (kind == _base.H4_EVENT and code == _base.EVT_LE_META
                and len(body) >= 3 and body[0] == LE_PERIODIC_SYNC_LOST):
            got = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            if got == sync_handle:
                _base._restore(scanner, deferred)
                return
        deferred.append(packet)

    _base._restore(scanner, deferred)
    raise HciError("timed out waiting for Periodic Advertising Sync Lost")


def run_recovery_phase(book, label, advertiser, scanner):
    sync_handle = None
    handles = None
    step = "initial periodic sync"
    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)
        _configure_periodic(advertiser, adv_type)
        _start_periodic(advertiser)

        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)

        # prepare_controller() starts with HCI Reset. Reset the synchronized
        # receiver once while the periodic train remains active, then prove it
        # can rebuild the sync and receive immediately.
        step = "periodic sync after scanner reset"
        prepare_controller(scanner)
        sync_handle = None
        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)

        # Reset the active advertiser while the scanner keeps its existing
        # synchronization. The old train must end first; wait for the scanner's
        # Sync Lost event so the old and rebuilt trains are separated by the
        # Controller's real asynchronous state transition rather than a delay
        # or a second scanner Reset. Then rebuild the advertiser and prove the
        # unchanged scanner can acquire the new train.
        step = "periodic sync lost after advertiser reset"
        old_sync = sync_handle
        adv_id, adv_type, _ = prepare_controller(advertiser)
        _wait_sync_lost(scanner, old_sync)
        sync_handle = None

        step = "periodic sync after advertiser reset"
        _configure_periodic(advertiser, adv_type)
        _start_periodic(advertiser)
        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)
        _terminate_sync(scanner, sync_handle)
        sync_handle = None
        _stop_periodic(advertiser)

        # Return both Controllers to ordinary connection state after the
        # advanced-state resets and prove bidirectional HCI still works.
        step = "ACL recovery after advanced-state resets"
        peripheral_id, peripheral_type, _ = prepare_controller(scanner)
        _, central_type, _ = prepare_controller(advertiser)
        start_legacy_advertising(
            scanner, peripheral_id, peripheral_type, True
        )
        start_legacy_connection(
            advertiser, central_type, peripheral_id, peripheral_type
        )
        central_handle, peripheral_handle = wait_acl_pair(advertiser, scanner)
        handles = central_handle, peripheral_handle
        advertiser.send_acl(central_handle, RECOVERY_CID, RECOVERY_MARKER)
        if not wait_acl_marker(
            scanner, peripheral_handle, RECOVERY_CID, RECOVERY_MARKER
        ):
            raise HciError("ACL marker missing after advanced-state recovery")

        book.passed(
            "Recovery",
            "reset/reconnect after active advanced procedures",
            "%s scanner reset + advertiser reset + ACL recovery" % label,
        )
    except (HciError, HciGone) as err:
        book.failed(
            "Recovery",
            "reset/reconnect after active advanced procedures",
            "%s: %s" % (step, err),
        )
    finally:
        if handles is not None:
            disconnect_acl_pair(
                advertiser, scanner, handles[0], handles[1]
            )
        if sync_handle is not None:
            _terminate_sync(scanner, sync_handle)
        _stop_periodic(advertiser)

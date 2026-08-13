#!/usr/bin/env python3
"""Recovery checks after active periodic advertising/synchronization state."""

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

OP_RESET = 0x0C03
RECOVERY_CID = 0x0040
RECOVERY_MARKER = b"HCI recovery ACL"


def _sync_once(advertiser, scanner, adv_id, adv_type):
    _create_periodic_sync(scanner, adv_id, adv_type)
    sync, _ = _wait_sync_established(scanner)
    _wait_periodic_report(scanner, sync, PERIODIC_MARKER)
    return sync


def run_recovery_phase(book, label, advertiser, scanner):
    sync_handle = None
    handles = None
    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)
        _configure_periodic(advertiser, adv_type)
        _start_periodic(advertiser)

        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)

        # Reset the synchronized receiver while the periodic train remains
        # active, then prove it can immediately rebuild the sync and receive.
        scanner.command(OP_RESET)
        prepare_controller(scanner)
        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)

        # Reset the active advertiser while the receiver has a sync. Rebuild
        # the advertising state from reset, reset the receiver's stale sync
        # state, and prove the train can be acquired again.
        advertiser.command(OP_RESET)
        adv_id, adv_type, _ = prepare_controller(advertiser)
        _configure_periodic(advertiser, adv_type)
        _start_periodic(advertiser)
        prepare_controller(scanner)
        sync_handle = _sync_once(advertiser, scanner, adv_id, adv_type)
        _terminate_sync(scanner, sync_handle)
        sync_handle = None
        _stop_periodic(advertiser)

        # Return both Controllers to ordinary connection state after the
        # advanced-state resets and prove bidirectional HCI still works.
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
            str(err),
        )
    finally:
        if handles is not None:
            disconnect_acl_pair(
                advertiser, scanner, handles[0], handles[1]
            )
        if sync_handle is not None:
            _terminate_sync(scanner, sync_handle)
        _stop_periodic(advertiser)

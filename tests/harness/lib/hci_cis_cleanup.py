#!/usr/bin/env python3
"""
Teardown for the two-controller CIS run.

Kept out of hci_cis_pair_test so it can be driven against a recorded
controller. A hardware run ended with LE Set Host Feature answering Command
Disallowed on both dongles and the test still printing PASS, which is the
kind of thing only a test of the teardown catches.

Imports no serial library, so that regression runs anywhere python does.
"""

import struct
import sys
import time

from hci_events import (
    EVT_DISCONNECTION_COMPLETE,
    H4_EVENT,
    HciError,
    HciGone,
    status_text,
)

OP_DISCONNECT = 0x0406
OP_LE_SET_ADV_ENABLE = 0x200A
OP_LE_REMOVE_CIG = 0x2065
OP_LE_REMOVE_ISO_DATA_PATH = 0x206F
OP_LE_SET_HOST_FEATURE = 0x2074

# Vol 6 Part B 4.6: bit 32 is Connected Isochronous Stream, Host Support.
CIS_HOST_SUPPORT_BIT = 32

# Remote User Terminated Connection, Vol 4 Part D 2.
DISCONNECT_REASON = 0x13


def remove_iso_paths(hci, cis):
    hci.command(
        OP_LE_REMOVE_ISO_DATA_PATH,
        struct.pack("<HB", cis, 0x03),
        allow_fail=True,
    )


def drain(hcis, seconds):
    """Read and discard whatever the controllers have queued."""
    end = time.time() + seconds
    while time.time() < end:
        for hci in hcis:
            try:
                hci.read_packet(0.02)
            except (HciError, HciGone):
                pass


def wait_disconnected(hci, handle, timeout=5.0):
    """
    Read until Disconnection Complete for this handle, or give up.

    Returns True when the link is confirmed gone. Command Status only says
    the controller accepted the request; the link is still up until the
    event arrives, and LE Set Host Feature is refused for as long as it is.
    The default exceeds the 4 second supervision timeout used by the pair
    tests, so a valid terminal event is not rejected by a shorter host timer.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            packet = hci.read_packet(0.1)
        except (HciError, HciGone):
            return False
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_DISCONNECTION_COMPLETE:
            continue
        if len(body) < 3:
            continue
        if struct.unpack("<H", body[1:3])[0] & 0x0FFF == handle:
            return True
    return False


def cleanup(central, peripheral, cig_id, central_cis, peripheral_cis,
            central_acl, peripheral_acl):
    for hci, cis in ((central, central_cis), (peripheral, peripheral_cis)):
        if cis is None:
            continue
        try:
            remove_iso_paths(hci, cis)
        except (HciError, HciGone):
            pass

    if central_cis is not None:
        try:
            central.command(
                OP_DISCONNECT,
                struct.pack("<HB", central_cis, DISCONNECT_REASON),
                allow_fail=True,
            )
        except (HciError, HciGone):
            pass

    drain((central, peripheral), 0.5)

    if cig_id is not None:
        try:
            central.command(OP_LE_REMOVE_CIG, bytes([cig_id]), allow_fail=True)
        except (HciError, HciGone):
            pass

    for hci, acl in ((central, central_acl), (peripheral, peripheral_acl)):
        if acl is None:
            continue
        try:
            hci.command(
                OP_DISCONNECT,
                struct.pack("<HB", acl, DISCONNECT_REASON),
                allow_fail=True,
            )
        except (HciError, HciGone):
            pass

    try:
        peripheral.command(OP_LE_SET_ADV_ENABLE, b"\x00", allow_fail=True)
    except (HciError, HciGone):
        pass

    # Vol 4 Part E 7.8.115: LE Set Host Feature is refused with Command
    # Disallowed while the controller has a connection. Disconnect only
    # returns Command Status, so without waiting for the event the clear
    # below is sent into a live link and fails every run, leaving CIS Host
    # Support set on both dongles for whatever runs next.
    for hci, acl in ((central, central_acl), (peripheral, peripheral_acl)):
        if acl is not None:
            wait_disconnected(hci, acl)

    for label, hci in (("central", central), ("peripheral", peripheral)):
        try:
            status, _ = hci.command(
                OP_LE_SET_HOST_FEATURE,
                bytes([CIS_HOST_SUPPORT_BIT, 0]),
                allow_fail=True,
            )
        except (HciError, HciGone):
            continue
        # Teardown must not fail the run, but a teardown that never works
        # should not look like one that did.
        if status != 0:
            print("Warning: %s left CIS Host Support set, clear returned %s"
                  % (label, status_text(status)), file=sys.stderr)

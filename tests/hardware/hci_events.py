#!/usr/bin/env python3
"""
Decoding for the HCI events the hardware tests read.

Byte handling only. Nothing here opens a port or imports pyserial, so the
regressions that feed it captured event bodies run anywhere python does,
including a machine with no serial library and no board attached.
"""

import struct

# LE Meta subevent codes, Vol 4 Part E 7.7.65.
LE_CONNECTION_COMPLETE = 0x01
LE_ADVERTISING_REPORT = 0x02
LE_CONNECTION_UPDATE_COMPLETE = 0x03
LE_READ_REMOTE_FEATURES_COMPLETE = 0x04
LE_LONG_TERM_KEY_REQUEST = 0x05
LE_DATA_LENGTH_CHANGE = 0x07
LE_ENHANCED_CONNECTION_COMPLETE = 0x0A
LE_PHY_UPDATE_COMPLETE = 0x0C
LE_EXTENDED_ADVERTISING_REPORT = 0x0D
LE_ENHANCED_CONNECTION_COMPLETE_V2 = 0x29

# Vol 4 Part E 7.7.65.1 for the legacy form, 7.7.65.10 for the enhanced one.
LE_CONNECTION_COMPLETE_LEN = 18
LE_ENHANCED_CONNECTION_COMPLETE_LEN = 30


def parse_connection(body):
    """
    Pull the fields the tests use out of a connection complete meta event.

    Handles the legacy form and both enhanced versions. v2 appends
    Advertising_Handle and Sync_Handle to the v1 body and moves nothing, so
    the two share a branch and the same minimum length; an event cut short
    after the interval triple is still usable because the appended fields are
    never read here.

    Returns None rather than raising for anything it cannot read. The
    advertise, connect and probe loops feed every meta event through this, so
    a malformed one has to be ignorable without unwinding the loop.
    """
    if not body:
        return None

    sub = body[0]
    if sub == LE_CONNECTION_COMPLETE:
        if len(body) < LE_CONNECTION_COMPLETE_LEN:
            return None
        interval, latency, timeout = struct.unpack("<HHH", body[12:18])
    elif sub in (LE_ENHANCED_CONNECTION_COMPLETE,
                 LE_ENHANCED_CONNECTION_COMPLETE_V2):
        if len(body) < LE_ENHANCED_CONNECTION_COMPLETE_LEN:
            return None
        # Two resolvable private addresses sit between the peer address and
        # the interval triple, which is the whole difference from the legacy
        # layout.
        interval, latency, timeout = struct.unpack("<HHH", body[24:30])
    else:
        return None

    status = body[1]
    handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
    role = body[4]
    peer = body[6:12]
    return status, handle, role, peer, interval, latency, timeout


def describe_interval(interval, latency, timeout):
    return "interval %.2f ms, latency %d, timeout %d ms" % (
        interval * 1.25, latency, timeout * 10)

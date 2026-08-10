#!/usr/bin/env python3
"""
The HCI vocabulary the hardware tests share: packet and event codes, error
names, the exceptions they raise, and the decoding of connection events.

Byte handling only. Nothing here opens a port or imports pyserial, so the
regressions that feed it captured bodies or drive its logic against a
recorded controller run anywhere python does, with no board attached.
"""

import struct

# H:4 packet indicators, Vol 4 Part A 2.
H4_COMMAND = 0x01
H4_ACL = 0x02
H4_SCO = 0x03
H4_EVENT = 0x04
H4_ISO = 0x05

# Event codes, Vol 4 Part E 7.7.
EVT_DISCONNECTION_COMPLETE = 0x05
EVT_ENCRYPTION_CHANGE = 0x08
EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_NUM_COMPLETED_PACKETS = 0x13
EVT_LE_META = 0x3E

# Vol 4 Part D 2. Only the codes these tests can actually provoke.
ERROR_NAMES = {
    0x00: "Success",
    0x01: "Unknown HCI Command",
    0x02: "Unknown Connection Identifier",
    0x08: "Connection Timeout",
    0x0C: "Command Disallowed",
    0x11: "Unsupported Feature or Parameter Value",
    0x12: "Invalid HCI Command Parameters",
    0x13: "Remote User Terminated Connection",
    0x16: "Connection Terminated By Local Host",
    0x3E: "Connection Failed To Be Established",
    0x41: "Unacceptable Connection Parameters",
    0x42: "Unknown Advertising Identifier",
    0x43: "Limit Reached",
    0x44: "Operation Cancelled By Host",
}


class HciError(Exception):
    pass


class HciGone(Exception):
    """
    The port went away underneath us.

    On a dongle this is not a serial problem. MPSL and the SoftDevice
    Controller reset the chip from their assert handlers by design, see
    HciNrf52840MpslAssert in src/hci_nrf52840.cpp, so a controller fault
    takes the USB device with it and the CDC port disappears. The board then
    re-enumerates and the next run starts clean, which is exactly what makes
    it easy to mistake for a flaky cable.
    """


def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))

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

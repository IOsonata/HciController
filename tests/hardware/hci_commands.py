#!/usr/bin/env python3
"""
Every HCI command the firmware dispatches, with a payload that can be sent.

The firmware answers 150 opcodes. Before this file the Python tooling drove
29 of them, so everything added after the ACL credit fix had only ever met a
compiled stub. This is the table that closes that: one entry per opcode, with
a parameter block that is valid, a note on what answer to expect, and what
has to exist first for the command to be worth sending.

Three tools read it:

    hci_ble_test.py probe       sends each one at a real controller
    tests/command_coverage.py   checks it against the dispatch table
    fake_controller.py          answers each one with no board attached

What is deliberately not here: the length of each return block. The C++ test
tests/unit/hci_sdc_dispatch_test.cpp already checks all 150 against the
vendor headers, and a second copy of those numbers in Python would be a
second thing to keep right. This file answers a different question, which is
whether the command works on a radio.

Fields
    opcode      the 16 bit opcode
    name        as the specification writes it
    reply       COMPLETE, STATUS or NONE, what the controller must send back
    payload     bytes, or a callable taking the probe context
    needs       what must exist first, see NEEDS_*. One value or several:
                Disconnect needs both a connection and consent, and asking
                for only one of the two sends it at a handle that is not
                there
    phase       which advertising mode the command belongs to, see PHASE_*
    undo        an (opcode, payload) pair that puts the controller back, or
                None when the command changes nothing that lasts
    undo_now    send the undo immediately rather than at the end of the
                phase, for state that blocks the command after it
    expect      status codes other than success that are a correct answer to
                this parameter block, so a probe run can tell a controller
                behaving properly from one that is not
    note        why the payload is what it is, where that is not obvious
    retry_after a list of (opcode, payload, why) candidates. When the row is
                refused for a reason it did not predict, each is applied in
                turn and the row sent again, and the run reports which one
                made it work. One run separates several explanations, rather
                than one run per guess. State accumulates on purpose: a
                candidate that only works with an earlier one in place is an
                answer too, and the report names all of them
"""

import struct

COMPLETE = "complete"
STATUS = "status"
NONE = "none"

# Nothing has to exist first. Safe on a controller straight out of reset.
NEEDS_NOTHING = ""

# Needs a connection handle. Skipped unless the caller supplies one.
NEEDS_CONN = "connection"

# Needs a periodic advertising set that exists and is configured.
NEEDS_ADV_SET = "advertising set"

# Needs a periodic sync. Nothing can make one without a second radio.
NEEDS_SYNC = "periodic sync"

# Changes the identity of the board or leaves the radio transmitting. Only
# sent when the caller asks for it.
NEEDS_CONSENT = "consent"

# Only the central may send it. A probe that got its link by advertising is
# the peripheral, so these are Command Disallowed and rightly so. Reaching
# them needs the probe to connect outward to a peer that will advertise.
NEEDS_CENTRAL = "central role"

# A handle no connection will ever have, for commands that are checked for
# their reply shape rather than their effect.
UNUSED_HANDLE = 0x0EFF

# The advertising set the probe configures and tears down.
PROBE_ADV_HANDLE = 0x00

# A controller is in legacy advertising mode or extended advertising mode,
# not both, and the specification says so in Vol 4 Part E 3.1.1. Whichever
# set of commands the host uses first, the other set is Command Disallowed
# until the controller is reset. Scanning and initiating go the same way.
#
# This is not optional and it is not a Nordic quirk. A run that sends
# LE Set Advertising Enable and then LE Set Extended Advertising Parameters
# gets Command Disallowed for everything extended that follows, which reads
# exactly like a controller missing half its commands. It is not: it is a
# host doing something a host is not allowed to do.
#
# So the rows are split, and the probe resets between the two groups.
PHASE_ANY = ""
PHASE_LEGACY = "legacy"
PHASE_EXTENDED = "extended"


# Statuses a correct controller returns often enough to be worth naming.
STATUS_UNKNOWN_CONNECTION = 0x02
STATUS_COMMAND_DISALLOWED = 0x0C
STATUS_UNSUPPORTED_FEATURE = 0x11
STATUS_INVALID_PARAMS = 0x12
STATUS_UNKNOWN_ADV_ID = 0x42


def _conn(ctx, tail=b""):
    return struct.pack("<H", ctx.handle) + tail


def _legacy_adv_params(ctx, adv_type=0x03):
    """
    Fifteen octets, not sixteen: there is no trailing field after the filter
    policy. Type 3 is non connectable undirected, so enabling it later cannot
    let anything in.

    The type is a parameter because LE Set Advertising Enable sends this
    again with other types as retry candidates. A non connectable type is
    also non scannable, and scan response data belongs to a scannable one, so
    whether the type and the data disagree is a question this can ask.
    """
    return struct.pack("<HHBBB6sBB", 0x00A0, 0x00F0, adv_type,
                       ctx.addr_type, 0x00, b"\x00" * 6, 0x07, 0x00)


class Command(object):
    __slots__ = ("opcode", "name", "reply", "payload", "needs", "undo",
                 "undo_now", "expect", "phase", "note", "retry_after")

    def __init__(self, opcode, name, reply, payload, needs=NEEDS_NOTHING,
                 undo=None, undo_now=False, expect=(), phase=PHASE_ANY,
                 note="", retry_after=None):
        self.opcode = opcode
        self.name = name
        self.reply = reply
        self.payload = payload
        self.needs = (needs,) if isinstance(needs, str) else tuple(needs)
        self.undo = undo
        self.undo_now = undo_now
        self.expect = expect
        self.phase = phase
        self.note = note
        # What to send and retry with when this row is refused for a reason
        # the row did not predict. Names the state the refusal points at, so
        # a run says whether that was what was missing instead of leaving it
        # to be guessed at between runs.
        self.retry_after = retry_after

    def build(self, ctx):
        """The parameter block to send, resolved against the probe context."""
        if callable(self.payload):
            return self.payload(ctx)
        return self.payload

    def __repr__(self):
        return "Command(0x%04X, %r)" % (self.opcode, self.name)


# ---------------------------------------------------------------------------
# Controller and baseband, 0x0Cxx
# ---------------------------------------------------------------------------

_CB = [
    Command(0x0C01, "Set Event Mask", COMPLETE,
            bytes.fromhex("ffffffffffffff3f"),
            undo=(0x0C01, bytes.fromhex("ffffffffffffff3f")),
            note="every event this specification version defines"),
    Command(0x0C03, "Reset", COMPLETE, b"",
            note="sent first by the probe, not as one of the tested rows"),
    Command(0x0C63, "Set Event Mask Page 2", COMPLETE, bytes(8),
            undo=(0x0C63, bytes(8)),
            note="all zeroes exercise the eight-octet page 2 command without "
                 "changing asynchronous event delivery during the probe"),
    Command(0x0C2D, "Read Transmit Power Level", COMPLETE,
            lambda ctx: _conn(ctx, b"\x00"), needs=NEEDS_CONN,
            note="type 0 is the current level"),
    # Controller to host flow control, 0x0C31, 0x0C33 and 0x0C35, is not here.
    # The controller answers Host Buffer Size with 0x11 and no sdc_support call
    # turns it on, so the firmware no longer dispatches or advertises the
    # three. Driving them here would only test that an unsupported command is
    # refused, which is not what these rows are for. See the note beside the
    # command table in src/hci_sdc_nrfxlib.cpp.
    Command(0x0C7B, "Read Authenticated Payload Timeout", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x0C7C, "Write Authenticated Payload Timeout", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<H", 3000)), needs=NEEDS_CONN,
            note="3000 units of 10 ms, the default"),
]

# ---------------------------------------------------------------------------
# Link control, 0x04xx
# ---------------------------------------------------------------------------

_LC = [
    Command(0x0406, "Disconnect", STATUS,
            lambda ctx: _conn(ctx, b"\x13"),
            needs=(NEEDS_CONSENT, NEEDS_CONN),
            note="0x13 is remote user terminated. Ends the link the probe is "
                 "using, so it runs last and only when asked"),
    Command(0x041D, "Read Remote Version Information", STATUS,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
]

# ---------------------------------------------------------------------------
# Informational and status parameters, 0x10xx and 0x14xx
# ---------------------------------------------------------------------------

_INFO = [
    Command(0x1001, "Read Local Version Information", COMPLETE, b""),
    Command(0x1002, "Read Local Supported Commands", COMPLETE, b""),
    Command(0x1003, "Read Local Supported Features", COMPLETE, b""),
    Command(0x1009, "Read BD_ADDR", COMPLETE, b""),
    Command(0x1405, "Read RSSI", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
]

# ---------------------------------------------------------------------------
# LE, the parts that need nothing but a reset controller
# ---------------------------------------------------------------------------

_ADV_DATA = bytes([2, 0x01, 0x06]) + bytes(29)
_SCAN_RSP = bytes([2, 0x01, 0x06]) + bytes(29)

_LE_BASIC = [
    Command(0x2001, "LE Set Event Mask", COMPLETE,
            bytes.fromhex("ffffffffffffff1f"),
            undo=(0x2001, bytes.fromhex("ffffffffffffff1f"))),
    Command(0x2002, "LE Read Buffer Size", COMPLETE, b""),
    Command(0x2003, "LE Read Local Supported Features", COMPLETE, b""),
    Command(0x2005, "LE Set Random Address", COMPLETE,
            bytes.fromhex("0102030405c0"),
            note="the top two bits set makes it a static random address"),
    Command(0x2006, "LE Set Advertising Parameters", COMPLETE,
            _legacy_adv_params,
            note="fifteen octets, not sixteen: there is no trailing field "
                 "after the filter policy. Type 3 is non connectable "
                 "undirected, so enabling it later cannot let anything in",
            phase=PHASE_LEGACY),
    Command(0x2007, "LE Read Advertising Physical Channel Tx Power", COMPLETE,
            b""),
    Command(0x2008, "LE Set Advertising Data", COMPLETE, _ADV_DATA,
            phase=PHASE_LEGACY),
    Command(0x2009, "LE Set Scan Response Data", COMPLETE, _SCAN_RSP,
            phase=PHASE_LEGACY),
    Command(0x200A, "LE Set Advertising Enable", COMPLETE, b"\x01",
            undo=(0x200A, b"\x00"),
            retry_after=[
                (0x2009, bytes(32), "the scan response data cleared"),
                (0x2008, bytes(32), "the advertising data cleared"),
                (0x2006, lambda ctx: _legacy_adv_params(ctx, adv_type=0x02),
                 "the type scannable undirected"),
                (0x2006, lambda ctx: _legacy_adv_params(ctx, adv_type=0x00),
                 "the type connectable undirected"),
                (0x202D, b"\x00", "address resolution off"),
            ],
            note="turned back off by undo, so the probe does not walk away "
                 "with the radio transmitting.\n"
                 "\n"
                 "This row is refused with Invalid HCI Command Parameters "
                 "on a board whose identity is a static random address, "
                 "repeatably, with and without consent.\n"
                 "\n"
                 "The header gives four conditions for that error and all "
                 "four are about the address. The one that fits, "
                 "Own_Address_Type random and the random address never "
                 "initialised, has been ruled out twice over: sending "
                 "LE Set Random Address again changes nothing, and "
                 "LE Set Scan Enable two rows below succeeds with the same "
                 "Own_Address_Type, which the header says needs that same "
                 "address. So the address is there and the error is not the "
                 "one the header names.\n"
                 "\n"
                 "Running without consent still fails, so the direct test "
                 "mode rows, VS Zephyr Write BD_ADDR and the broadcast rows "
                 "are all out. Address resolution off, the isochronous "
                 "group removed, the parameters sent again and the address "
                 "sent again are all out too, each tried in one run.\n"
                 "\n"
                 "The parameter block is not the subject either. "
                 "--only 0x2006,0x2008,0x2009,0x200A sends these four rows "
                 "and nothing else, with the same payloads, and all four are "
                 "accepted. Clearing the scan response data, clearing the "
                 "advertising data and sending the parameters with a "
                 "scannable and then a connectable type all leave it "
                 "refused.\n"
                 "\n"
                 "--bisect 0x200A named the row, twice, identically: "
                 "LE Read Maximum Advertising Data Length, 0x203A. Five "
                 "commands reproduced it. It was not the controller.\n"
                 "\n"
                 "sdk-nrf answers it. subsys/bluetooth/controller/"
                 "hci_internal.c holds the switch that tracks which "
                 "advertising command set a host has used since reset, and "
                 "0x203A is in it, marked ADV_COMMAND_TYPE_EXTENDED, beside "
                 "Set Extended Advertising Parameters. So it is an extended "
                 "advertising command and this table had it in the phase "
                 "that means neither. That is fixed where the row is "
                 "defined.\n"
                 "\n"
                 "What is left is a real gap, and it is ours. That file "
                 "answers a host mixing the two sets with Command "
                 "Disallowed, before the command reaches the controller. "
                 "This firmware calls the SoftDevice Controller directly and "
                 "has no such state, so a host that mixes them gets whatever "
                 "the controller does about it, which here was Invalid HCI "
                 "Command Parameters on the enable and nothing on the three "
                 "rows before it. The specification asks for 0x0C and we "
                 "pass through 0x12.\n"
                 "\n"
                 "The undo below is still worth having, since a run that "
                 "walks away advertising is a run that leaves the radio on",
            phase=PHASE_LEGACY),
    Command(0x200B, "LE Set Scan Parameters", COMPLETE,
            lambda ctx: struct.pack("<BHHBB", 0x00, 0x0060, 0x0030,
                                    ctx.addr_type, 0x00),
            note="passive, so the probe does not answer anything it hears. "
                 "The address type has to be one the board has: naming the "
                 "public address on a controller with none is accepted here "
                 "and refused by Set Scan Enable, so the failure lands on "
                 "the command after the one that is wrong",
            phase=PHASE_LEGACY),
    Command(0x200C, "LE Set Scan Enable", COMPLETE, b"\x01\x00",
            undo=(0x200C, b"\x00\x00"),
            phase=PHASE_LEGACY),
    Command(0x200E, "LE Create Connection Cancel", COMPLETE, b"",
            needs=NEEDS_CONSENT, expect=(STATUS_COMMAND_DISALLOWED,),
            note="Command Disallowed when nothing is being connected, which "
                 "is the normal state and a correct answer. It is also the "
                 "undo for both Create Connection commands",
            phase=PHASE_LEGACY),
    Command(0x200F, "LE Read Filter Accept List Size", COMPLETE, b""),
    Command(0x2010, "LE Clear Filter Accept List", COMPLETE, b""),
    Command(0x2011, "LE Add Device To Filter Accept List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0"),
            undo=(0x2010, b"")),
    Command(0x2012, "LE Remove Device From Filter Accept List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0")),
    Command(0x2014, "LE Set Host Channel Classification", COMPLETE,
            bytes.fromhex("ffffffff1f"),
            note="all 37 data channels allowed, the state after reset"),
    Command(0x2017, "LE Encrypt", COMPLETE, bytes(16) + bytes(16),
            note="a zero key on zero plaintext, so the answer is a constant "
                 "the caller can recognise"),
    Command(0x2018, "LE Rand", COMPLETE, b""),
    Command(0x201F, "LE Test End", COMPLETE, b"", needs=NEEDS_CONSENT,
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="Command Disallowed outside direct test mode, which is a "
                 "correct answer. Every test command ends its own test, so "
                 "by the time this row is reached there is none running"),
    Command(0x2023, "LE Read Suggested Default Data Length", COMPLETE, b""),
    Command(0x2024, "LE Write Suggested Default Data Length", COMPLETE,
            struct.pack("<HH", 251, 2120),
            undo=(0x2024, struct.pack("<HH", 27, 328)),
            note="undo restores the values a reset would have left"),
    Command(0x202F, "LE Read Maximum Data Length", COMPLETE, b""),
    Command(0x2031, "LE Set Default PHY", COMPLETE, b"\x00\x07\x07",
            undo=(0x2031, b"\x03\x00\x00"),
            note="all PHYs allowed both ways, then back to no preference"),
    # These three read nothing and change nothing, and all three are extended
    # advertising commands. sdk-nrf says so itself, in
    # subsys/bluetooth/controller/hci_internal.c, where the switch that tracks
    # which set the host has used since reset lists LE_READ_MAX_ADV_DATA_LENGTH,
    # LE_READ_NUMBER_OF_SUPPORTED_ADV_SETS and LE_READ_PERIODIC_ADV_LIST_SIZE
    # alongside Set Extended Advertising Parameters, and marks them
    # ADV_COMMAND_TYPE_EXTENDED.
    #
    # They sat in the phase that means neither set for as long as this table
    # existed, and sending one made every legacy advertising row below it
    # fail. Six guesses and eight hardware runs went to that before the
    # sdk-nrf source was read.
    Command(0x203A, "LE Read Maximum Advertising Data Length", COMPLETE, b"",
            phase=PHASE_EXTENDED),
    Command(0x203B, "LE Read Number Of Supported Advertising Sets", COMPLETE,
            b"", phase=PHASE_EXTENDED),
    Command(0x203D, "LE Clear Advertising Sets", COMPLETE, b"",
            phase=PHASE_EXTENDED),
    Command(0x204A, "LE Read Periodic Advertiser List Size", COMPLETE, b"",
            phase=PHASE_EXTENDED),
    Command(0x204B, "LE Read Transmit Power", COMPLETE, b""),
    Command(0x204C, "LE Read RF Path Compensation", COMPLETE, b""),
    Command(0x204D, "LE Write RF Path Compensation", COMPLETE,
            struct.pack("<hh", 0, 0),
            note="zero both ways, which is what a board with no external "
                 "front end wants and what reset leaves"),
    Command(0x2074, "LE Set Host Feature", COMPLETE, b"\x26\x01",
            undo=(0x2074, b"\x26\x00"),
            note="bit 38, Connection Subrating (Host Support), counting from "
                 "LE Encryption at zero. Bit 32 is Isochronous Channels "
                 "(Host Support), which this firmware does not enable, so "
                 "asking for it gets Unsupported Feature and says nothing "
                 "about subrating"),
    Command(0x2087, "LE Read All Local Supported Features", COMPLETE, b""),
    Command(0x2097, "LE Set Host Feature v2", COMPLETE,
            struct.pack("<HB", 38, 1),
            undo=(0x2097, struct.pack("<HB", 38, 0)),
            note="the same host-support bit used by v1, with the two-octet "
                 "bit number carried by the v2 command"),
    Command(0x207D, "LE Set Default Subrate", COMPLETE,
            struct.pack("<HHHHH", 1, 1, 0, 0, 300),
            note="a factor of one is no subrating, so this configures the "
                 "feature without changing how any link behaves"),
]

# ---------------------------------------------------------------------------
# LE privacy and the resolving list
# ---------------------------------------------------------------------------

# Not zeroes, deliberately. sdk-nrfxlib limitations.rst DRGN-9083 says an
# all zero identity resolving key in the resolving list makes any resolvable
# address that resolves against it report as that device, so a run using zero
# keys would be testing the erratum rather than the commands.
_PEER_IRK = bytes(range(16))
_LOCAL_IRK = bytes(range(16, 32))

_LE_PRIVACY = [
    Command(0x2027, "LE Add Device To Resolving List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0") + _PEER_IRK + _LOCAL_IRK,
            undo=(0x2029, b"")),
    Command(0x202A, "LE Read Resolving List Size", COMPLETE, b""),
    Command(0x204E, "LE Set Privacy Mode", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0") + b"\x00",
            note="network privacy, the default, on the entry just added"),
    Command(0x202D, "LE Set Address Resolution Enable", COMPLETE, b"\x01",
            undo=(0x202D, b"\x00")),
    Command(0x202E, "LE Set Resolvable Private Address Timeout", COMPLETE,
            struct.pack("<H", 900),
            undo=(0x202E, struct.pack("<H", 900)),
            note="900 seconds is the value reset leaves"),
    Command(0x2028, "LE Remove Device From Resolving List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0"),
            note="removes what Add put there, so the list ends empty"),
    Command(0x2029, "LE Clear Resolving List", COMPLETE, b""),
]

# ---------------------------------------------------------------------------
# LE extended advertising and scanning
# ---------------------------------------------------------------------------

_EXT_ADV_PARAMS = (
    struct.pack("<B", PROBE_ADV_HANDLE)
    + struct.pack("<H", 0x0000)              # non connectable, non scannable
    + b"\xa0\x00\x00"                        # primary interval min
    + b"\xf0\x00\x00"                        # primary interval max
    + b"\x07"                                # all three primary channels
    + b"\x01"                                # own address type, random
    + b"\x00" + bytes(6)                     # peer address, unused
    + b"\x00"                                # no filtering
    + struct.pack("<b", 0x7F)                # host has no power preference
    + b"\x01"                                # primary PHY, LE 1M
    + b"\x00"                                # no secondary max skip
    + b"\x01"                                # secondary PHY, LE 1M
    + b"\x00"                                # advertising SID
    + b"\x00"                                # no scan request notifications
)

_LE_EXT = [
    Command(0x2036, "LE Set Extended Advertising Parameters", COMPLETE,
            _EXT_ADV_PARAMS,
            note="creates the set every later row uses. It is not undone "
                 "here: TEARDOWN removes it, after the undo pass has stopped "
                 "it advertising",
            phase=PHASE_EXTENDED),
    Command(0x207F, "LE Set Extended Advertising Parameters v2", COMPLETE,
            _EXT_ADV_PARAMS + b"\x00\x00",
            needs=NEEDS_ADV_SET,
            note="the v1 block with primary and secondary PHY options on the "
                 "end. Sent against the set 0x2036 already made, so it "
                 "reconfigures rather than creating a second one",
            phase=PHASE_EXTENDED),
    Command(0x2035, "LE Set Advertising Set Random Address", COMPLETE,
            bytes([PROBE_ADV_HANDLE]) + bytes.fromhex("0102030405c0"),
            needs=NEEDS_ADV_SET,
            note="after the set exists, not before. A set advertising with a "
                 "random own address type and no address set is refused at "
                 "enable, not here",
            phase=PHASE_EXTENDED),
    Command(0x207C, "LE Set Data Related Address Changes", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x00]), needs=NEEDS_ADV_SET,
            note="change nothing, on a set that has to exist first",
            phase=PHASE_EXTENDED),
    Command(0x2037, "LE Set Extended Advertising Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 0x01, 3]) + bytes([2, 0x01, 0x06]),
            note="operation 3 is a complete block, fragment preference 1 is "
                 "do not fragment",
            phase=PHASE_EXTENDED),
    Command(0x2038, "LE Set Extended Scan Response Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 0x01, 0]),
            note="empty, because the set is not scannable",
            phase=PHASE_EXTENDED),
    Command(0x2041, "LE Set Extended Scan Parameters", COMPLETE,
            bytes([0x01, 0x00, 0x01]) + struct.pack("<BHH", 0x00, 0x10, 0x10),
            note="one PHY, LE 1M, passive",
            phase=PHASE_EXTENDED),
    Command(0x2042, "LE Set Extended Scan Enable", COMPLETE,
            struct.pack("<BBHH", 1, 0, 0, 0),
            undo=(0x2042, struct.pack("<BBHH", 0, 0, 0, 0)),
            phase=PHASE_EXTENDED),
]

# ---------------------------------------------------------------------------
# LE periodic advertising, PAST and PAwR
# ---------------------------------------------------------------------------

_LE_PERIODIC = [
    Command(0x203E, "LE Set Periodic Advertising Parameters", COMPLETE,
            struct.pack("<BHHH", PROBE_ADV_HANDLE, 0x0060, 0x00A0, 0x0000),
            needs=NEEDS_ADV_SET,
            note="properties zero, so no transmit power in the header",
            phase=PHASE_EXTENDED),
    Command(0x2086, "LE Set Periodic Advertising Parameters v2", COMPLETE,
            struct.pack("<BHHHBBBBB", PROBE_ADV_HANDLE, 0x0060, 0x00A0,
                        0x0000, 0, 0, 0, 0, 0),
            needs=NEEDS_ADV_SET,
            note="twelve octets. The trailing number of response slots is a "
                 "field of its own, so leaving it off is one short and the "
                 "controller rejects the parameters. Zero subevents is "
                 "periodic advertising without responses, which is the only "
                 "v2 call that needs no second radio",
            phase=PHASE_EXTENDED),
    Command(0x203F, "LE Set Periodic Advertising Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 3]) + bytes([2, 0xFF, 0x59]),
            needs=NEEDS_ADV_SET,
            note="one manufacturer specific byte, Nordic's company id low "
                 "octet, so the block is well formed",
            phase=PHASE_EXTENDED),
    Command(0x2039, "LE Set Extended Advertising Enable", COMPLETE,
            bytes([0x01, 0x01, PROBE_ADV_HANDLE]) + struct.pack("<HB", 0, 0),
            undo=(0x2039, b"\x00\x00"),
            note="no duration and no event limit, then disabled by undo",
            phase=PHASE_EXTENDED),
    Command(0x2040, "LE Set Periodic Advertising Enable", COMPLETE,
            bytes([0x01, PROBE_ADV_HANDLE]), needs=NEEDS_ADV_SET,
            undo=(0x2040, bytes([0x00, PROBE_ADV_HANDLE])),
            phase=PHASE_EXTENDED),
    Command(0x2045, "LE Periodic Advertising Create Sync Cancel", COMPLETE,
            b"", needs=NEEDS_CONSENT, expect=(STATUS_COMMAND_DISALLOWED,),
            note="Command Disallowed when no sync is pending, which is a "
                 "correct answer. It is also the undo for Create Sync",
            phase=PHASE_EXTENDED),
    Command(0x2047, "LE Add Device To Periodic Advertiser List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0") + b"\x00",
            undo=(0x2049, b"")),
    Command(0x2048, "LE Remove Device From Periodic Advertiser List", COMPLETE,
            b"\x00" + bytes.fromhex("0102030405c0") + b"\x00"),
    Command(0x2049, "LE Clear Periodic Advertiser List", COMPLETE, b""),
    Command(0x205D, "LE Set Default Periodic Advertising Sync Transfer "
                    "Parameters", COMPLETE,
            struct.pack("<BHHB", 0, 0, 0x000A, 0),
            note="mode 0 is no reporting, which changes nothing about a link"),
    Command(0x2059, "LE Set Periodic Advertising Receive Enable", COMPLETE,
            struct.pack("<HB", UNUSED_HANDLE, 0), needs=NEEDS_SYNC,
            phase=PHASE_EXTENDED),
    Command(0x205A, "LE Periodic Advertising Sync Transfer", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<HH", 0, UNUSED_HANDLE)),
            needs=NEEDS_SYNC,
            phase=PHASE_EXTENDED),
    Command(0x205B, "LE Periodic Advertising Set Info Transfer", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<HB", 0, PROBE_ADV_HANDLE)),
            needs=NEEDS_CONN, expect=(STATUS_UNKNOWN_ADV_ID,),
            note="names an advertising set that has to be running a "
                 "periodic train. The set the probe advertises from to get "
                 "its link is connectable and has none, so Unknown "
                 "Advertising Identifier is the right answer to it",
            phase=PHASE_EXTENDED),
    Command(0x205C, "LE Set Periodic Advertising Sync Transfer Parameters",
            COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<BHHB", 0, 0, 0x000A, 0)),
            needs=NEEDS_CONN),
    Command(0x2044, "LE Periodic Advertising Create Sync", STATUS,
            b"\x00\x00\x00" + bytes.fromhex("0102030405c0")
            + struct.pack("<HHB", 0, 0x0100, 0),
            needs=NEEDS_CONSENT, undo=(0x2045, b""), undo_now=True,
            note="fourteen octets: options, advertising SID, address type, "
                 "address, skip, sync timeout, sync CTE type. The timeout is "
                 "in 10 ms units and 0x000A is the floor, which a controller "
                 "rejects for a train it has to find first; 0x0100 is 2.56 "
                 "seconds. It starts a scan, so it is cancelled at once",
            phase=PHASE_EXTENDED),
    Command(0x2046, "LE Periodic Advertising Terminate Sync", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE), needs=NEEDS_SYNC,
            phase=PHASE_EXTENDED),
    Command(0x2082, "LE Set Periodic Advertising Subevent Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 1, 0, 0, 0, 0]), needs=NEEDS_ADV_SET,
            expect=(STATUS_UNKNOWN_ADV_ID, STATUS_COMMAND_DISALLOWED),
            note="one subevent, no response slots, no data. The parameter "
                 "block is a walk of variable size entries rather than an "
                 "array, which is what makes it worth sending",
            phase=PHASE_EXTENDED),
    Command(0x2083, "LE Set Periodic Advertising Response Data", COMPLETE,
            struct.pack("<HHBBBB", UNUSED_HANDLE, 0, 0, 0, 0, 0),
            needs=NEEDS_SYNC,
            phase=PHASE_EXTENDED),
    Command(0x2084, "LE Set Periodic Sync Subevent", COMPLETE,
            struct.pack("<HHB", UNUSED_HANDLE, 0, 0), needs=NEEDS_SYNC,
            phase=PHASE_EXTENDED),
]

# ---------------------------------------------------------------------------
# LE power control, path loss and the rest of the connection scoped set
# ---------------------------------------------------------------------------

_LE_CONN = [
    Command(0x200D, "LE Create Connection", STATUS,
            struct.pack("<HHBB6sBHHHHHH", 0x0060, 0x0030, 0, 0,
                        bytes.fromhex("0102030405c0"), 0,
                        0x0018, 0x0028, 0, 0x02BC, 0, 0),
            needs=NEEDS_CONSENT, undo=(0x200E, b""), undo_now=True,
            note="an address nothing answers to, so this starts an initiator "
                 "that is cancelled at once. Left running it is Command "
                 "Disallowed for everything that touches the radio after it",
            phase=PHASE_LEGACY),
    Command(0x2013, "LE Connection Update", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<HHHHHH", 0x0018, 0x0028, 0,
                                               0x02BC, 0, 0)),
            needs=NEEDS_CONN,
            expect=(STATUS_UNSUPPORTED_FEATURE,),
            note="30 to 50 ms, a seven second supervision timeout, no "
                 "connection event length preference. Answered 0x11 on a "
                 "link this board is the peripheral of, against a phone. "
                 "Why is not established: it is recorded as an answer this "
                 "row has seen rather than one it understands, and a run "
                 "where the board is the central would settle it"),
    Command(0x2015, "LE Read Channel Map", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x2016, "LE Read Remote Features", STATUS,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x2019, "LE Enable Encryption", STATUS,
            lambda ctx: _conn(ctx, bytes(8) + bytes(2) + bytes(16)),
            needs=(NEEDS_CONSENT, NEEDS_CONN, NEEDS_CENTRAL),
            note="a zero long term key. The peer will not agree, so this is "
                 "only about whether the command reaches the link layer. "
                 "Central only, so a probe that got its link by advertising "
                 "cannot reach it"),
    Command(0x201A, "LE Long Term Key Request Reply", COMPLETE,
            lambda ctx: _conn(ctx, bytes(16)),
            needs=(NEEDS_CONSENT, NEEDS_CONN),
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="only valid while an LE Long Term Key Request is "
                 "outstanding, which happens when a central offers to pair "
                 "with a link this board is the peripheral of. Command "
                 "Disallowed at any other time is correct. The probe sends "
                 "the negative reply first when a request is outstanding, "
                 "because this key is zeros and encrypting with it drops "
                 "the link on the integrity check"),
    Command(0x201B, "LE Long Term Key Request Negative Reply", COMPLETE,
            lambda ctx: _conn(ctx), needs=(NEEDS_CONSENT, NEEDS_CONN),
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="refuses pairing without ending the link, so it is what "
                 "the probe answers a real request with. Command Disallowed "
                 "when there is none outstanding is equally correct"),
    Command(0x2022, "LE Set Data Length", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<HH", 251, 2120)),
            needs=NEEDS_CONN),
    Command(0x2030, "LE Read PHY", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x2032, "LE Set PHY", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<BBBH", 0, 0x07, 0x07, 0)),
            needs=NEEDS_CONN),
    Command(0x2043, "LE Extended Create Connection", STATUS,
            b"\x00\x00\x00" + bytes.fromhex("0102030405c0") + b"\x01"
            + struct.pack("<HHHHHHHH", 0x0060, 0x0030, 0x0018, 0x0028, 0,
                          0x02BC, 0, 0),
            needs=NEEDS_CONSENT, undo=(0x200E, b""), undo_now=True,
            note="one PHY set in the mask, so exactly one parameter group "
                 "follows. Getting that count wrong is the mistake this "
                 "command exists to catch",
            phase=PHASE_EXTENDED),
    Command(0x2085, "LE Extended Create Connection v2", STATUS,
            bytes([PROBE_ADV_HANDLE, 0x00])
            + b"\x00\x00\x00" + bytes.fromhex("0102030405c0") + b"\x01"
            + struct.pack("<HHHHHHHH", 0x0060, 0x0030, 0x0018, 0x0028, 0,
                          0x02BC, 0, 0),
            needs=(NEEDS_ADV_SET, NEEDS_CONSENT), undo=(0x200E, b""),
            undo_now=True,
            note="the periodic advertising with responses form: an "
                 "advertising handle and a subevent ahead of the v1 block. "
                 "One PHY set in the mask, so one parameter group follows",
            phase=PHASE_EXTENDED),
    Command(0x2076, "LE Enhanced Read Transmit Power Level", COMPLETE,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN,
            note="PHY 1 is LE 1M"),
    Command(0x2077, "LE Read Remote Transmit Power Level", STATUS,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN),
    Command(0x2078, "LE Set Path Loss Reporting Parameters", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<BBBBH", 70, 10, 50, 10, 5)),
            needs=NEEDS_CONN,
            note="high threshold, high hysteresis, low threshold, low "
                 "hysteresis, minimum time spent. High means high path "
                 "loss, so it is the larger number: 70 dB over 50 dB with "
                 "10 dB of hysteresis either side. Written the other way "
                 "round it is a block the controller has to reject, and it "
                 "was, and then Set Path Loss Reporting Enable was "
                 "Command Disallowed after it"),
    Command(0x2079, "LE Set Path Loss Reporting Enable", COMPLETE,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN,
            undo=None),
    Command(0x207A, "LE Set Transmit Power Reporting Enable", COMPLETE,
            lambda ctx: _conn(ctx, b"\x01\x01"), needs=NEEDS_CONN),
    Command(0x206D, "LE Request Peer SCA", STATUS,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x207E, "LE Subrate Request", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<HHHHH", 1, 1, 0, 0, 300)),
            needs=NEEDS_CONN,
            note="a factor of one, so the peer can agree without the link "
                 "timing changing"),
    Command(0x2088, "LE Read All Remote Features", STATUS,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN,
            note="three octets. The handle then one count of pages "
                 "requested, not a page number and a count"),
]

# ---------------------------------------------------------------------------
# Isochronous channels, connected and broadcast.
#
# Only two things here can be reached without a second radio: the group
# configuration commands, which build a CIG out of nothing and take it away
# again, and the three vendor settings. Everything else wants a stream, and a
# stream wants a peer, so those rows are sent at a handle no stream will ever
# have and are checked for their reply shape. A controller that answers
# Unknown Connection Identifier has parsed the block, found the field and
# looked the handle up, which is the whole of what can be tested from one
# board.
#
# Every broadcast row below asks for an unencrypted group. Not because the
# rows are about encryption, but because they are not: an encrypted request
# on this part would be answered by whatever the controller does about
# nRF52840 not being one of the nRF52 devices sdk-nrfxlib lists as
# encrypting isochronous packets, and that answer says nothing about whether
# the parameter block was parsed, which is what these rows measure.
# ---------------------------------------------------------------------------

# The group and the stream the CIG rows build. Removing the group is what
# takes the stream with it.
PROBE_CIG_ID = 0x00
PROBE_CIG_TEST_ID = 0x01
PROBE_CIS_ID = 0x00

# The broadcast groups the BIG rows name. Two, because LE Create BIG and
# LE Create BIG Test both build one and a group handle holds one group.
PROBE_BIG_HANDLE = 0x00
PROBE_BIG_TEST_HANDLE = 0x01

# Coding format 0x03 in the assigned numbers. Not zero: zero is mu-law, a
# real codec, and a controller that does not implement it says so.
CODING_FORMAT_TRANSPARENT = 0x03


def _u24(value):
    return struct.pack("<I", value)[:3]


_ISO = [
    Command(0x2060, "LE Read Buffer Size v2", COMPLETE, b"",
            note="version 1 reports the ACL length and count and says "
                 "nothing about isochronous packets, so a host cannot flow "
                 "control a stream without this one"),
    Command(0xFD19, "VS CIG Reserved Time Set", COMPLETE,
            struct.pack("<I", 1300), undo=(0xFD19, struct.pack("<I", 1300)),
            note="1300 us is the default the header states. Sent before any "
                 "CIG is built, because the header says it applies to groups "
                 "created after it and is kept across an HCI Reset, so a "
                 "value left behind here outlives the run"),
    Command(0xFD1A, "VS CIS Subevent Length Set", COMPLETE,
            struct.pack("<I", 0), undo=(0xFD1A, struct.pack("<I", 0)),
            note="zero leaves the length to the controller, which is the "
                 "default. Also kept across a reset"),
    Command(0xFD18, "VS BIG Reserved Time Set", COMPLETE,
            struct.pack("<I", 1600), undo=(0xFD18, struct.pack("<I", 1600)),
            note="1600 us is the default the header states"),
    Command(0x2062, "LE Set CIG Parameters", COMPLETE,
            bytes([PROBE_CIG_ID]) + _u24(10000) + _u24(10000)
            + bytes([0, 0, 0]) + struct.pack("<HH", 20, 20)
            + bytes([1])
            + bytes([PROBE_CIS_ID]) + struct.pack("<HH", 40, 40)
            + bytes([1, 1, 2, 2]),
            note="fifteen octets then one nine octet stream. Both SDU "
                 "intervals are 10 ms in a 24 bit field, worst case clock "
                 "accuracy 0, sequential packing, unframed, 20 ms of "
                 "transport latency either way. The stream is 40 octets each "
                 "way on LE 1M with two retransmissions. Latency below the "
                 "ISO interval is unschedulable for an unframed group, so it "
                 "is twice the SDU interval rather than equal to it.\n"
                 "\n"
                 "10 ms is also chosen against sdk-nrfxlib limitations.rst, "
                 "DRGN-21099: this controller enforces framed units unless "
                 "the SDU interval is an integer multiple of 1250 us or an "
                 "integer divisor of 5000 us, and once framed it refuses "
                 "unequal intervals and anything under 5000 us. 10000 is "
                 "eight times 1250, so unframed stands and neither of those "
                 "applies"),
    Command(0x2064, "LE Create CIS", STATUS,
            lambda ctx: bytes([1]) + struct.pack("<HH", UNUSED_HANDLE,
                                                 ctx.handle),
            needs=(NEEDS_CONN, NEEDS_CENTRAL),
            expect=(STATUS_UNKNOWN_CONNECTION,),
            note="only the central may start a stream, and the stream handle "
                 "comes back from Set CIG Parameters, which this table does "
                 "not read. So the ACL handle is real and the stream handle "
                 "is not: what is tested is that the array is walked and "
                 "both handles are looked up"),
    Command(0x2065, "LE Remove CIG", COMPLETE, bytes([PROBE_CIG_ID]),
            note="takes away the group Set CIG Parameters built, so the "
                 "configuration does not outlive the run"),
    Command(0x2063, "LE Set CIG Parameters Test", COMPLETE,
            bytes([PROBE_CIG_TEST_ID]) + _u24(10000) + _u24(10000)
            + bytes([1, 1]) + struct.pack("<H", 8) + bytes([0, 0, 0])
            + bytes([1])
            + bytes([PROBE_CIS_ID, 2]) + struct.pack("<HHHH", 40, 40, 40, 40)
            + bytes([1, 1, 1, 1]),
            undo=(0x2065, bytes([PROBE_CIG_TEST_ID])), undo_now=True,
            note="a second group, so it does not collide with the one above. "
                 "The test form states what the other form derives: flush "
                 "timeout 1 either way, ISO interval 8 in 1.25 ms units which "
                 "is the 10 ms the SDU interval asks for, two subevents, one "
                 "burst either way. Removed at once, since a group left "
                 "configured is Command Disallowed for the next thing that "
                 "wants the same identifier"),
    Command(0x2066, "LE Accept CIS Request", STATUS,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="the peripheral half. Answering a request that was never "
                 "made is the only form of it reachable from one board"),
    Command(0x2067, "LE Reject CIS Request", COMPLETE,
            struct.pack("<HB", UNUSED_HANDLE, 0x0D),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="reason 0x0D, rejected due to limited resources"),
    Command(0x2061, "LE Read ISO TX Sync", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED)),
    Command(0x2075, "LE Read ISO Link Quality", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED)),
    Command(0xFD17, "VS ISO Read Tx Timestamp", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="the vendor form of LE Read ISO TX Sync, without the time "
                 "offset"),
    Command(0x206E, "LE Setup ISO Data Path", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE) + bytes([0, 0])
            + bytes([CODING_FORMAT_TRANSPARENT]) + bytes(4)
            + _u24(0) + bytes([0]),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="input direction, HCI data path, transparent air mode, no "
                 "controller delay, no codec configuration. The trailing "
                 "length is what makes the block variable, so a zero there "
                 "is the shortest legal form of it. The coding format was "
                 "five zero octets once, and zero is not transparent, it is "
                 "mu-law: the controller answered Unsupported Feature or "
                 "Parameter Value, which is the correct answer to a codec "
                 "it does not implement"),
    Command(0x206F, "LE Remove ISO Data Path", COMPLETE,
            struct.pack("<HB", UNUSED_HANDLE, 0x01),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="a bit mask of directions, not a direction, so 0x01 is the "
                 "input path alone"),
    Command(0x2070, "LE ISO Transmit Test", COMPLETE,
            struct.pack("<HB", UNUSED_HANDLE, 0),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="payload type 0, zero length. These four are the ones worth "
                 "having on an instrument: they measure a stream with no "
                 "codec anywhere"),
    Command(0x2071, "LE ISO Receive Test", COMPLETE,
            struct.pack("<HB", UNUSED_HANDLE, 0),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED)),
    Command(0x2072, "LE ISO Read Test Counters", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED)),
    Command(0x2073, "LE ISO Test End", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED)),
    Command(0x2068, "LE Create BIG", STATUS,
            bytes([PROBE_BIG_HANDLE, PROBE_ADV_HANDLE, 1]) + _u24(10000)
            + struct.pack("<HH", 40, 20) + bytes([2, 1, 0, 0, 0]) + bytes(16),
            needs=(NEEDS_ADV_SET, NEEDS_CONSENT),
            expect=(STATUS_UNKNOWN_ADV_ID, STATUS_COMMAND_DISALLOWED),
            note="one broadcast stream of 40 octets every 10 ms, LE 1M, two "
                 "retransmissions, unencrypted, so the broadcast code is "
                 "sixteen zero octets that are still sent. A group needs "
                 "periodic advertising running on the set, which nothing "
                 "here leaves running, so being refused is the answer this "
                 "row expects. The 10 ms SDU interval also clears "
                 "sdk-nrfxlib limitations.rst DRGN-21246, which says this "
                 "command does not support an interval under 1250 us, or "
                 "under 5000 us with framed units",
            phase=PHASE_EXTENDED),
    Command(0x2069, "LE Create BIG Test", STATUS,
            bytes([PROBE_BIG_TEST_HANDLE, PROBE_ADV_HANDLE, 1]) + _u24(10000)
            + struct.pack("<H", 8) + bytes([1]) + struct.pack("<HH", 40, 40)
            + bytes([1, 0, 0, 1, 1, 0, 0]) + bytes(16),
            needs=(NEEDS_ADV_SET, NEEDS_CONSENT),
            undo=(0x206A, bytes([PROBE_BIG_TEST_HANDLE, 0x16])),
            expect=(STATUS_UNKNOWN_ADV_ID,),
            note="the same group stated rather than derived: ISO interval 8 "
                 "in 1.25 ms units, one subevent, 40 octet service and "
                 "protocol units, one burst, one repeated transmission, no "
                 "pre transmission offset. The subevent count was two, and "
                 "the header says the immediate repetition count has to be "
                 "in the range 1 to NSE divided by BN, so two subevents with "
                 "one burst and one repetition leaves a subevent nothing "
                 "fills.\n"
                 "\n"
                 "It is refused, and the refusal is the answer this row "
                 "wants. LE Create BIG above it succeeds and takes the "
                 "periodic train, and a train holds one group only. The "
                 "group handle here is a free one, so a run that still "
                 "refused told the two causes apart: it did, with Unknown "
                 "Advertising Identifier, which names the train rather than "
                 "the handle. Reaching this one with a train of its own "
                 "would need a second periodic advertising set, and "
                 "HCI_SDC_PERIODIC_ADV_COUNT is one",
            phase=PHASE_EXTENDED),
    Command(0x206A, "LE Terminate BIG", STATUS,
            bytes([PROBE_BIG_HANDLE, 0x16]),
            expect=(STATUS_UNKNOWN_ADV_ID, STATUS_COMMAND_DISALLOWED),
            note="reason 0x16, terminated by local host",
            phase=PHASE_EXTENDED),
    Command(0x206B, "LE BIG Create Sync", STATUS,
            bytes([PROBE_BIG_HANDLE]) + struct.pack("<H", UNUSED_HANDLE)
            + bytes([0]) + bytes(16) + bytes([0])
            + struct.pack("<H", 100) + bytes([1, 1]),
            needs=NEEDS_SYNC,
            note="the receiving half, which needs a periodic sync to a "
                 "broadcaster and so needs a second radio. Timeout 100 in "
                 "10 ms units, one stream requested, which is index 1: the "
                 "list is one based and a zero there is rejected",
            phase=PHASE_EXTENDED),
    Command(0x206C, "LE BIG Terminate Sync", COMPLETE,
            bytes([PROBE_BIG_HANDLE]),
            expect=(STATUS_UNKNOWN_ADV_ID, STATUS_COMMAND_DISALLOWED),
            phase=PHASE_EXTENDED),
]

# ---------------------------------------------------------------------------
# Direct test mode. Every entry leaves the radio transmitting or receiving,
# so the probe pairs each with LE Test End.
# ---------------------------------------------------------------------------

_DTM = [
    Command(0x201D, "LE Receiver Test v1", COMPLETE, b"\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True,
            note="every test command here ends its own test. A test left "
                 "running is Command Disallowed for the next one, and for "
                 "advertising and scanning as well, so deferring the undo "
                 "to the end of the phase loses everything after it"),
    Command(0x201E, "LE Transmitter Test v1", COMPLETE, b"\x00\x25\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True,
            note="channel 0, 37 octets, PRBS9"),
    Command(0x2033, "LE Receiver Test v2", COMPLETE, b"\x00\x01\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True),
    Command(0x2034, "LE Transmitter Test v2", COMPLETE, b"\x00\x25\x00\x01",
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True),
    Command(0x204F, "LE Receiver Test v3", COMPLETE,
            b"\x00\x01\x00\x00\x00\x01\x00", needs=NEEDS_CONSENT,
            undo=(0x201F, b""), undo_now=True,
            note="seven octets before the antenna identifiers: channel, PHY, "
                 "modulation index, expected CTE length and type, slot "
                 "durations, switching pattern length. No constant tone "
                 "extension, so the pattern length is zero and nothing "
                 "follows it"),
    Command(0x2050, "LE Transmitter Test v3", COMPLETE,
            b"\x00\x25\x00\x01\x00\x00\x00", needs=NEEDS_CONSENT,
            undo=(0x201F, b""), undo_now=True),
    Command(0x207B, "LE Transmitter Test v4", COMPLETE,
            b"\x00\x25\x00\x01\x00\x00\x00" + struct.pack("<b", 0x7F),
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True,
            note="the transmit power octet comes after the antenna "
                 "identifiers, not before them. v4 is the only test command "
                 "with a field past the variable part"),
    Command(0xFD23, "VS Transmitter Carrier Test", COMPLETE,
            b"\x00" + struct.pack("<b", 0),
            needs=NEEDS_CONSENT, undo=(0x201F, b""), undo_now=True,
            note="an unmodulated carrier, which is what a regulatory "
                 "measurement wants and what nothing else here produces"),
]

# ---------------------------------------------------------------------------
# Vendor specific, Zephyr layout at 0xFCxx and Nordic at 0xFDxx
# ---------------------------------------------------------------------------

_VENDOR = [
    Command(0xFC01, "VS Zephyr Read Version Info", COMPLETE, b""),
    Command(0xFC02, "VS Zephyr Read Supported Commands", COMPLETE, b"",
            note="the firmware masks this to what it dispatches, so a bit "
                 "set here is a promise the coverage check holds it to"),
    Command(0xFC06, "VS Zephyr Write BD_ADDR", COMPLETE,
            bytes.fromhex("0102030405c0"), needs=NEEDS_CONSENT,
            undo=(0xFC06, lambda ctx: ctx.public_addr), undo_now=True,
            note="changes the public address of the board. The note here "
                 "said until it is reset, and the header says otherwise: "
                 "the address is written to volatile memory, does not change "
                 "during an HCI Reset, and is cleared only by a system "
                 "reset. So a run without an undo left the board answering a "
                 "different identity to everything, across resets and across "
                 "later runs, until it was unplugged.\n"
                 "\n"
                 "That is not only untidy. A board with no public address "
                 "advertises with the random one, and a board that this "
                 "command has given a public address advertises with that "
                 "instead, so the run after it exercises a different path "
                 "and a failure on the first path stops being reachable. "
                 "The undo puts back whatever Read BD_ADDR answered before "
                 "the run started, which is six zero octets on a board that "
                 "never had one. Immediate, because every row below it in "
                 "this phase would otherwise run against the wrong "
                 "identity"),
    Command(0xFC09, "VS Zephyr Read Static Addresses", COMPLETE, b""),
    Command(0xFC0A, "VS Zephyr Read Key Hierarchy Roots", COMPLETE, b"",
            note="returns the identity and encryption roots, which is why "
                 "it is worth knowing the command answers at all"),
    Command(0xFC0B, "VS Zephyr Read Chip Temperature", COMPLETE, b""),
    Command(0xFC0E, "VS Zephyr Write Tx Power", COMPLETE,
            struct.pack("<BHb", 0, PROBE_ADV_HANDLE, 0), needs=NEEDS_ADV_SET,
            note="handle type 0 is advertising, and for extended advertising "
                 "the handle names the set, so the set has to exist. 0 dBm",
            phase=PHASE_EXTENDED),
    Command(0xFC0F, "VS Zephyr Read Tx Power", COMPLETE,
            struct.pack("<BH", 0, PROBE_ADV_HANDLE), needs=NEEDS_ADV_SET,
            phase=PHASE_EXTENDED),
    Command(0xFD01, "VS LLPM Mode Set", COMPLETE, b"\x00",
            note="off. Low latency packet mode is a Nordic extension that "
                 "only talks to another Nordic controller"),
    Command(0xFD02, "VS Connection Update", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<IHH", 30000, 0, 300)),
            needs=NEEDS_CONN,
            expect=(STATUS_UNSUPPORTED_FEATURE,),
            note="the interval is microseconds in a 32 bit field, not the "
                 "1.25 ms units the specification uses, which is the point "
                 "of the vendor command. 30000 us, and the header says the "
                 "range is 7500 to 4000000 in 1250 us steps"),
    Command(0xFD04, "VS QoS Connection Event Report Enable", COMPLETE,
            b"\x00", undo=(0xFD04, b"\x00")),
    Command(0xFD0C, "VS Set Advertising Randomness", COMPLETE,
            struct.pack("<BH", PROBE_ADV_HANDLE, 0),
            needs=NEEDS_ADV_SET,
            note="zero extra randomness, the state after reset",
            phase=PHASE_EXTENDED),
    Command(0xFD0E, "VS QoS Channel Survey Enable", COMPLETE,
            struct.pack("<BI", 1, 100000),
            undo=(0xFD0E, struct.pack("<BI", 0, 0)), undo_now=True,
            note="on, with a 100 ms average measurement interval, which the "
                 "header says must be between 3000 and 4000000 us. Undo "
                 "turns it off. Disabling something never enabled is "
                 "Command Disallowed, so the order is the wrong way round "
                 "from what it looks like"),
    Command(0xFD11, "VS Read Average RSSI", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0xFD14, "VS Get Next Connection Event Counter", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0xFD1F, "VS Connection Anchor Point Update Event Report Enable",
            COMPLETE, b"\x00", undo=(0xFD1F, b"\x00")),
    Command(0xFFF0, "VS Read Counters", COMPLETE, b"",
            note="this firmware's own tallies and the two pool figures. No "
                 "SDC call behind it"),
]

# The order matters. Everything that creates state comes before what uses it.
COMMANDS = (_CB + _LC + _INFO + _LE_BASIC + _LE_PRIVACY + _LE_EXT
            + _LE_PERIODIC + _LE_CONN + _ISO + _DTM + _VENDOR)

# Run after everything above, and after the undo entries have been replayed.
#
# An advertising set cannot be removed while it is advertising, and the undo
# entries are what stop it. So this cannot be an ordinary row: in the middle
# of the list it takes the set away from everything below it, and at the end
# of the list it runs before the undo pass and is refused. It gets its own
# pass, after both.
TEARDOWN = [
    Command(0x203C, "LE Remove Advertising Set", COMPLETE,
            bytes([PROBE_ADV_HANDLE]), needs=NEEDS_ADV_SET,
            expect=(STATUS_UNKNOWN_ADV_ID,),
            note="the set is already gone if Set Extended Advertising "
                 "Parameters never worked, and saying so is better than "
                 "hiding it",
            phase=PHASE_EXTENDED),
]

BY_OPCODE = {}
for _cmd in COMMANDS + TEARDOWN:
    if _cmd.opcode in BY_OPCODE:
        raise AssertionError("opcode 0x%04X listed twice" % _cmd.opcode)
    BY_OPCODE[_cmd.opcode] = _cmd

#!/usr/bin/env python3
"""
Every HCI command the firmware dispatches, with a payload that can be sent.

The firmware answers 126 opcodes. Before this file the Python tooling drove
29 of them, so everything added after the ACL credit fix had only ever met a
compiled stub. This is the table that closes that: one entry per opcode, with
a parameter block that is valid, a note on what answer to expect, and what
has to exist first for the command to be worth sending.

Three tools read it:

    hci_ble_test.py probe       sends each one at a real controller
    tests/command_coverage.py   checks it against the dispatch table
    fake_controller.py          answers each one with no board attached

What is deliberately not here: the length of each return block. The C++ test
tests/unit/hci_sdc_dispatch_test.cpp already checks all 126 against the
vendor headers, and a second copy of those numbers in Python would be a
second thing to keep right. This file answers a different question, which is
whether the command works on a radio.

Fields
    opcode      the 16 bit opcode
    name        as the specification writes it
    reply       COMPLETE, STATUS or NONE, what the controller must send back
    payload     bytes, or a callable taking the probe context
    needs       what must exist first, see NEEDS_*
    undo        an (opcode, payload) pair that puts the controller back, or
                None when the command changes nothing that lasts
    note        why the payload is what it is, where that is not obvious
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

# A handle no connection will ever have, for commands that are checked for
# their reply shape rather than their effect.
UNUSED_HANDLE = 0x0EFF

# The advertising set the probe configures and tears down.
PROBE_ADV_HANDLE = 0x00


def _conn(ctx, tail=b""):
    return struct.pack("<H", ctx.handle) + tail


class Command(object):
    __slots__ = ("opcode", "name", "reply", "payload", "needs", "undo", "note")

    def __init__(self, opcode, name, reply, payload, needs=NEEDS_NOTHING,
                 undo=None, note=""):
        self.opcode = opcode
        self.name = name
        self.reply = reply
        self.payload = payload
        self.needs = needs
        self.undo = undo
        self.note = note

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
    Command(0x0C2D, "Read Transmit Power Level", COMPLETE,
            lambda ctx: _conn(ctx, b"\x00"), needs=NEEDS_CONN,
            note="type 0 is the current level"),
    Command(0x0C31, "Set Controller To Host Flow Control", COMPLETE,
            b"\x00", undo=(0x0C31, b"\x00"),
            note="0 turns it off, which is where the probe leaves it"),
    Command(0x0C33, "Host Buffer Size", COMPLETE,
            struct.pack("<HBHH", 251, 0, 4, 0),
            note="ACL only, no synchronous buffers"),
    Command(0x0C35, "Host Number Of Completed Packets", NONE,
            struct.pack("<BHH", 1, UNUSED_HANDLE, 0),
            note="zero packets on a handle that has none, so the controller "
                 "has nothing to credit back. Answers nothing on success, "
                 "which is the whole point of testing it"),
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
            lambda ctx: _conn(ctx, b"\x13"), needs=NEEDS_CONSENT,
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
            struct.pack("<HHBBB6sBBB", 0x00A0, 0x00F0, 0x03, 0x00, 0x00,
                        b"\x00" * 6, 0x07, 0x00, 0x00),
            note="non connectable undirected, so enabling it later cannot "
                 "let anything in"),
    Command(0x2007, "LE Read Advertising Physical Channel Tx Power", COMPLETE,
            b""),
    Command(0x2008, "LE Set Advertising Data", COMPLETE, _ADV_DATA),
    Command(0x2009, "LE Set Scan Response Data", COMPLETE, _SCAN_RSP),
    Command(0x200A, "LE Set Advertising Enable", COMPLETE, b"\x01",
            undo=(0x200A, b"\x00"),
            note="turned back off by undo, so the probe does not walk away "
                 "with the radio transmitting"),
    Command(0x200B, "LE Set Scan Parameters", COMPLETE,
            struct.pack("<BHHBB", 0x00, 0x0010, 0x0010, 0x00, 0x00),
            note="passive, so the probe does not answer anything it hears"),
    Command(0x200C, "LE Set Scan Enable", COMPLETE, b"\x01\x00",
            undo=(0x200C, b"\x00\x00")),
    Command(0x200E, "LE Create Connection Cancel", COMPLETE, b"",
            needs=NEEDS_CONSENT,
            note="fails with 0x0C when nothing is being connected, which is "
                 "the normal state, so it is only sent on request"),
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
    Command(0x201F, "LE Test End", COMPLETE, b"",
            note="fails with 0x0C outside direct test mode, so the probe "
                 "sends it only after starting a test"),
    Command(0x2023, "LE Read Suggested Default Data Length", COMPLETE, b""),
    Command(0x2024, "LE Write Suggested Default Data Length", COMPLETE,
            struct.pack("<HH", 251, 2120),
            undo=(0x2024, struct.pack("<HH", 27, 328)),
            note="undo restores the values a reset would have left"),
    Command(0x202F, "LE Read Maximum Data Length", COMPLETE, b""),
    Command(0x2031, "LE Set Default PHY", COMPLETE, b"\x00\x07\x07",
            undo=(0x2031, b"\x03\x00\x00"),
            note="all PHYs allowed both ways, then back to no preference"),
    Command(0x203A, "LE Read Maximum Advertising Data Length", COMPLETE, b""),
    Command(0x203B, "LE Read Number Of Supported Advertising Sets", COMPLETE,
            b""),
    Command(0x203D, "LE Clear Advertising Sets", COMPLETE, b""),
    Command(0x204A, "LE Read Periodic Advertiser List Size", COMPLETE, b""),
    Command(0x204B, "LE Read Transmit Power", COMPLETE, b""),
    Command(0x204C, "LE Read RF Path Compensation", COMPLETE, b""),
    Command(0x204D, "LE Write RF Path Compensation", COMPLETE,
            struct.pack("<hh", 0, 0),
            note="zero both ways, which is what a board with no external "
                 "front end wants and what reset leaves"),
    Command(0x2074, "LE Set Host Feature", COMPLETE, b"\x20\x01",
            undo=(0x2074, b"\x20\x00"),
            note="bit 32 is connection subrating host support"),
    Command(0x207C, "LE Set Data Related Address Changes", COMPLETE,
            b"\x00\x00", note="advertising set 0, change nothing"),
    Command(0x207D, "LE Set Default Subrate", COMPLETE,
            struct.pack("<HHHHH", 1, 1, 0, 0, 300),
            note="a factor of one is no subrating, so this configures the "
                 "feature without changing how any link behaves"),
]

# ---------------------------------------------------------------------------
# LE privacy and the resolving list
# ---------------------------------------------------------------------------

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
    Command(0x2035, "LE Set Advertising Set Random Address", COMPLETE,
            bytes([PROBE_ADV_HANDLE]) + bytes.fromhex("0102030405c0"),
            note="must come after the set exists, so the probe orders it "
                 "after Set Extended Advertising Parameters"),
    Command(0x2036, "LE Set Extended Advertising Parameters", COMPLETE,
            _EXT_ADV_PARAMS, undo=(0x203D, b""),
            note="creates the set every later row uses, and undo clears it"),
    Command(0x2037, "LE Set Extended Advertising Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 0x01, 3]) + bytes([2, 0x01, 0x06]),
            note="operation 3 is a complete block, fragment preference 1 is "
                 "do not fragment"),
    Command(0x2038, "LE Set Extended Scan Response Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 0x01, 0]),
            note="empty, because the set is not scannable"),
    Command(0x2039, "LE Set Extended Advertising Enable", COMPLETE,
            bytes([0x01, 0x01, PROBE_ADV_HANDLE]) + struct.pack("<HB", 0, 0),
            undo=(0x2039, b"\x00\x00"),
            note="no duration and no event limit, then disabled by undo"),
    Command(0x2041, "LE Set Extended Scan Parameters", COMPLETE,
            bytes([0x01, 0x00, 0x01]) + struct.pack("<BHH", 0x00, 0x10, 0x10),
            note="one PHY, LE 1M, passive"),
    Command(0x2042, "LE Set Extended Scan Enable", COMPLETE,
            struct.pack("<BBHH", 1, 0, 0, 0),
            undo=(0x2042, struct.pack("<BBHH", 0, 0, 0, 0))),
    Command(0x203C, "LE Remove Advertising Set", COMPLETE,
            bytes([PROBE_ADV_HANDLE]),
            note="runs after everything that needs the set"),
]

# ---------------------------------------------------------------------------
# LE periodic advertising, PAST and PAwR
# ---------------------------------------------------------------------------

_LE_PERIODIC = [
    Command(0x203E, "LE Set Periodic Advertising Parameters", COMPLETE,
            struct.pack("<BHHH", PROBE_ADV_HANDLE, 0x0060, 0x00A0, 0x0000),
            needs=NEEDS_ADV_SET,
            note="properties zero, so no transmit power in the header"),
    Command(0x203F, "LE Set Periodic Advertising Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 0x03, 3]) + bytes([2, 0xFF, 0x59]),
            needs=NEEDS_ADV_SET,
            note="one manufacturer specific byte, Nordic's company id low "
                 "octet, so the block is well formed"),
    Command(0x2040, "LE Set Periodic Advertising Enable", COMPLETE,
            bytes([0x01, PROBE_ADV_HANDLE]), needs=NEEDS_ADV_SET,
            undo=(0x2040, bytes([0x00, PROBE_ADV_HANDLE]))),
    Command(0x2086, "LE Set Periodic Advertising Parameters v2", COMPLETE,
            struct.pack("<BHHHBBBB", PROBE_ADV_HANDLE, 0x0060, 0x00A0,
                        0x0000, 0, 0, 0, 0),
            needs=NEEDS_ADV_SET,
            note="zero subevents, which is periodic advertising without "
                 "responses and the only v2 call that needs no second radio"),
    Command(0x2045, "LE Periodic Advertising Create Sync Cancel", COMPLETE,
            b"", needs=NEEDS_CONSENT,
            note="fails with 0x0C when no sync is pending, so on request"),
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
            struct.pack("<HB", UNUSED_HANDLE, 0), needs=NEEDS_SYNC),
    Command(0x205A, "LE Periodic Advertising Sync Transfer", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<HH", 0, UNUSED_HANDLE)),
            needs=NEEDS_SYNC),
    Command(0x205B, "LE Periodic Advertising Set Info Transfer", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<HB", 0, PROBE_ADV_HANDLE)),
            needs=NEEDS_CONN),
    Command(0x205C, "LE Set Periodic Advertising Sync Transfer Parameters",
            COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<BHHB", 0, 0, 0x000A, 0)),
            needs=NEEDS_CONN),
    Command(0x2044, "LE Periodic Advertising Create Sync", STATUS,
            b"\x00\x00" + bytes.fromhex("0102030405c0")
            + struct.pack("<HHBB", 0, 0x000A, 0, 0),
            needs=NEEDS_CONSENT,
            note="starts a scan for a periodic train that is not there. "
                 "Cancel it, or it keeps the radio busy"),
    Command(0x2046, "LE Periodic Advertising Terminate Sync", COMPLETE,
            struct.pack("<H", UNUSED_HANDLE), needs=NEEDS_SYNC),
    Command(0x2082, "LE Set Periodic Advertising Subevent Data", COMPLETE,
            bytes([PROBE_ADV_HANDLE, 1, 0, 0, 0, 0]), needs=NEEDS_ADV_SET,
            note="one subevent, no response slots, no data. The parameter "
                 "block is a walk of variable size entries rather than an "
                 "array, which is what makes it worth sending"),
    Command(0x2083, "LE Set Periodic Advertising Response Data", COMPLETE,
            struct.pack("<HHBBBB", UNUSED_HANDLE, 0, 0, 0, 0, 0),
            needs=NEEDS_SYNC),
    Command(0x2084, "LE Set Periodic Sync Subevent", COMPLETE,
            struct.pack("<HHB", UNUSED_HANDLE, 0, 0), needs=NEEDS_SYNC),
]

# ---------------------------------------------------------------------------
# LE power control, path loss and the rest of the connection scoped set
# ---------------------------------------------------------------------------

_LE_CONN = [
    Command(0x200D, "LE Create Connection", STATUS,
            struct.pack("<HHBB6sBHHHHHH", 0x0060, 0x0030, 0, 0,
                        bytes.fromhex("0102030405c0"), 0,
                        0x0018, 0x0028, 0, 0x02BC, 0, 0),
            needs=NEEDS_CONSENT,
            note="an address nothing answers to, so this starts an initiator "
                 "that has to be cancelled"),
    Command(0x2013, "LE Connection Update", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<HHHHHH", 0x0018, 0x0028, 0,
                                               0x02BC, 0, 0)),
            needs=NEEDS_CONN,
            note="30 to 50 ms, which any peer accepts"),
    Command(0x2015, "LE Read Channel Map", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x2016, "LE Read Remote Features", STATUS,
            lambda ctx: _conn(ctx), needs=NEEDS_CONN),
    Command(0x2019, "LE Enable Encryption", STATUS,
            lambda ctx: _conn(ctx, bytes(8) + bytes(2) + bytes(16)),
            needs=NEEDS_CONSENT,
            note="a zero long term key. The peer will not agree, so this is "
                 "only about whether the command reaches the link layer"),
    Command(0x201A, "LE Long Term Key Request Reply", COMPLETE,
            lambda ctx: _conn(ctx, bytes(16)), needs=NEEDS_CONSENT),
    Command(0x201B, "LE Long Term Key Request Negative Reply", COMPLETE,
            lambda ctx: _conn(ctx), needs=NEEDS_CONSENT),
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
            needs=NEEDS_CONSENT,
            note="one PHY set in the mask, so exactly one parameter group "
                 "follows. Getting that count wrong is the mistake this "
                 "command exists to catch"),
    Command(0x2076, "LE Enhanced Read Transmit Power Level", COMPLETE,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN,
            note="PHY 1 is LE 1M"),
    Command(0x2077, "LE Read Remote Transmit Power Level", STATUS,
            lambda ctx: _conn(ctx, b"\x01"), needs=NEEDS_CONN),
    Command(0x2078, "LE Set Path Loss Reporting Parameters", COMPLETE,
            lambda ctx: _conn(ctx, struct.pack("<BBBBH", 50, 10, 70, 10, 5)),
            needs=NEEDS_CONN,
            note="a high threshold of 70 dB with 10 dB of hysteresis, so a "
                 "link on a bench sits in the low zone and reports once"),
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
            lambda ctx: _conn(ctx, b"\x00\x00"), needs=NEEDS_CONN,
            note="page zero, one page requested"),
]

# ---------------------------------------------------------------------------
# Direct test mode. Every entry leaves the radio transmitting or receiving,
# so the probe pairs each with LE Test End.
# ---------------------------------------------------------------------------

_DTM = [
    Command(0x201D, "LE Receiver Test v1", COMPLETE, b"\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b"")),
    Command(0x201E, "LE Transmitter Test v1", COMPLETE, b"\x00\x25\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b""),
            note="channel 0, 37 octets, PRBS9"),
    Command(0x2033, "LE Receiver Test v2", COMPLETE, b"\x00\x01\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b"")),
    Command(0x2034, "LE Transmitter Test v2", COMPLETE, b"\x00\x25\x00\x01",
            needs=NEEDS_CONSENT, undo=(0x201F, b"")),
    Command(0x204F, "LE Receiver Test v3", COMPLETE,
            b"\x00\x01\x00\x00\x00\x00", needs=NEEDS_CONSENT,
            undo=(0x201F, b""),
            note="no constant tone extension, so no antenna identifiers "
                 "follow and the switching pattern length is zero"),
    Command(0x2050, "LE Transmitter Test v3", COMPLETE,
            b"\x00\x25\x00\x01\x00\x00\x00", needs=NEEDS_CONSENT,
            undo=(0x201F, b"")),
    Command(0x207B, "LE Transmitter Test v4", COMPLETE,
            b"\x00\x25\x00\x01\x00\x00\x00" + struct.pack("<b", 0x7F),
            needs=NEEDS_CONSENT, undo=(0x201F, b""),
            note="the transmit power octet comes after the antenna "
                 "identifiers, not before them. v4 is the only test command "
                 "with a field past the variable part"),
    Command(0xFD23, "VS Transmitter Carrier Test", COMPLETE,
            b"\x00" + struct.pack("<b", 0) + b"\x00",
            needs=NEEDS_CONSENT, undo=(0x201F, b""),
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
            note="changes the identity of the board until it is reset"),
    Command(0xFC09, "VS Zephyr Read Static Addresses", COMPLETE, b""),
    Command(0xFC0A, "VS Zephyr Read Key Hierarchy Roots", COMPLETE, b"",
            note="returns the identity and encryption roots, which is why "
                 "it is worth knowing the command answers at all"),
    Command(0xFC0B, "VS Zephyr Read Chip Temperature", COMPLETE, b""),
    Command(0xFC0E, "VS Zephyr Write Tx Power", COMPLETE,
            struct.pack("<BHb", 0, 0, 0),
            note="role 0 is advertising, handle unused, 0 dBm"),
    Command(0xFC0F, "VS Zephyr Read Tx Power", COMPLETE,
            struct.pack("<BH", 0, 0)),
    Command(0xFD01, "VS LLPM Mode Set", COMPLETE, b"\x00",
            note="off. Low latency packet mode is a Nordic extension that "
                 "only talks to another Nordic controller"),
    Command(0xFD02, "VS Connection Update", STATUS,
            lambda ctx: _conn(ctx, struct.pack("<BHHH", 0, 24, 0, 300)),
            needs=NEEDS_CONN,
            note="an interval in units the specification does not have, "
                 "which is the point of the vendor command"),
    Command(0xFD04, "VS QoS Connection Event Report Enable", COMPLETE,
            b"\x00", undo=(0xFD04, b"\x00")),
    Command(0xFD0C, "VS Set Advertising Randomness", COMPLETE,
            struct.pack("<BH", PROBE_ADV_HANDLE, 0),
            needs=NEEDS_ADV_SET,
            note="zero extra randomness, the state after reset"),
    Command(0xFD0E, "VS QoS Channel Survey Enable", COMPLETE,
            struct.pack("<BI", 0, 0), undo=(0xFD0E, struct.pack("<BI", 0, 0)),
            note="disabled, with the interval that reset leaves. Enabling it "
                 "puts the radio on the air between links"),
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

# The order matters. Everything that creates state comes before what uses it,
# and the sets are torn down at the end by the undo entries.
COMMANDS = (_CB + _LC + _INFO + _LE_BASIC + _LE_PRIVACY + _LE_EXT
            + _LE_PERIODIC + _LE_CONN + _DTM + _VENDOR)

BY_OPCODE = {}
for _cmd in COMMANDS:
    if _cmd.opcode in BY_OPCODE:
        raise AssertionError("opcode 0x%04X listed twice" % _cmd.opcode)
    BY_OPCODE[_cmd.opcode] = _cmd

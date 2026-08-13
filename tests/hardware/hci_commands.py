#!/usr/bin/env python3
"""nRF52840 + sdk-nrfxlib HEAD command profile used by hardware tooling.

The complete nrfxlib-backed command catalog is kept in
hci_commands_catalog.py. This module applies the nRF52840 release profile and
adds commands implemented by the supplemental SDC dispatcher.
"""

import struct

import hci_commands_catalog as _catalog
from hci_commands_catalog import *

# Vol 4 Part E advertising-command-family rules put these in the extended set.
# Running them in PHASE_ANY lets an earlier legacy-family command select the
# other command family and makes the controller correctly answer 0x0C.
EXTENDED_PHASE_OPCODES = frozenset((
    0x2047,  # LE Add Device To Periodic Advertiser List
    0x2048,  # LE Remove Device From Periodic Advertiser List
    0x2049,  # LE Clear Periodic Advertiser List
    0x205C,  # LE Set Periodic Advertising Sync Transfer Parameters
    0x205D,  # LE Set Default Periodic Advertising Sync Transfer Parameters
))

for _opcode in EXTENDED_PHASE_OPCODES:
    _catalog.BY_OPCODE[_opcode].phase = PHASE_EXTENDED

# The broad probe deliberately does not build a PAwR synchronized state before
# driving Extended Create Connection v2. In that state Command Disallowed is a
# valid state-dependent response, just as it is for the neighboring PAwR rows.
# The two-radio harness owns the positive PAwR/0x2085 procedure.
_ext_create_v2 = _catalog.BY_OPCODE[0x2085]
if STATUS_COMMAND_DISALLOWED not in _ext_create_v2.expect:
    _ext_create_v2.expect = (
        tuple(_ext_create_v2.expect) + (STATUS_COMMAND_DISALLOWED,)
    )

# Core 6.2 commands routed by src/hci_sdc.cpp rather than the older vendor
# table. The first two use an unused handle so the probe verifies parsing and
# reply shape without changing a live connection. The default-rate command is
# last and intentionally invalid so it cannot change the controller defaults.
_CORE_62 = (
    Command(0x209D, "LE Frame Space Update", STATUS,
            struct.pack("<HHHBH", UNUSED_HANDLE, 150, 150, 0x01, 0x0001),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED,
                    STATUS_INVALID_PARAMS)),
    Command(0x20A1, "LE Connection Rate Request", STATUS,
            struct.pack("<H", UNUSED_HANDLE) + bytes(18),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED,
                    STATUS_INVALID_PARAMS)),
    Command(0x20A3, "LE Read Minimum Supported Connection Interval", COMPLETE,
            b"", note="read-only Core 6.2 capability query"),
    Command(0x20A2, "LE Set Default Rate Parameters", COMPLETE, bytes(18),
            expect=(STATUS_INVALID_PARAMS,),
            note="intentionally invalid and last, so probing cannot change "
                 "the controller's default connection-rate parameters"),
)

# Nordic SDC vendor commands that are present in the nRF52 multirole
# controller but were not routed by the older generated table. Values that are
# retained across HCI Reset are written back to Nordic's documented defaults so
# the broad probe does not leave the controller in a different configuration.
# DTM uses Test End only; the carrier-start subcommand is deliberately not part
# of an unattended probe.
_SDC_VS = (
    Command(0xFC1F, "VS DTM Command: Test End", COMPLETE, b"\x00",
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="safe DTM subcommand; never starts an RF carrier"),
    Command(0xFD03, "VS Connection Event Extend", COMPLETE, b"\x00",
            note="disable/default state"),
    Command(0xFD05, "VS Event Length Set", COMPLETE,
            struct.pack("<I", 7500),
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="Nordic default 7500 us; may be refused once a link exists"),
    Command(0xFD06, "VS Periodic Advertising Event Length Set", COMPLETE,
            struct.pack("<I", 7500),
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="Nordic default 7500 us; retained across HCI Reset"),
    Command(0xFD09, "VS Peripheral Latency Mode Set", COMPLETE,
            lambda ctx: struct.pack("<HB", ctx.handle, 0x00),
            needs=NEEDS_CONN,
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="mode 0 enables normal peripheral latency"),
    Command(0xFD0A, "VS Write Remote TX Power", STATUS,
            lambda ctx: struct.pack("<HBb", ctx.handle, 0x01, 0),
            needs=NEEDS_CONN,
            expect=(STATUS_COMMAND_DISALLOWED, STATUS_UNSUPPORTED_FEATURE),
            note="1M PHY, delta 0 requests information without a power change"),
    Command(0xFD0D, "VS Compatibility Mode Window Offset Set", COMPLETE,
            b"\x00", note="disabled/default state retained across HCI Reset"),
    Command(0xFD10, "VS Set Power Control Request Parameters", COMPLETE,
            struct.pack("<BBHbbbbHB", 0, 0, 2048,
                        -70, -30, -65, -35, 5000, 5),
            note="Nordic documented default power-control parameters"),
    Command(0xFD12, "VS Central ACL Event Spacing Set", COMPLETE,
            struct.pack("<I", 7500),
            expect=(STATUS_COMMAND_DISALLOWED,),
            note="Nordic default 7500 us; may be refused once initiating/link state exists"),
    Command(0xFD15, "VS Allow Parallel Connection Establishments", COMPLETE,
            b"\x00", note="disabled/default state"),
    Command(0xFD16, "VS Minimum Maximum ACL TX Payload Set", COMPLETE,
            b"\x1b", expect=(STATUS_COMMAND_DISALLOWED,),
            note="27-byte Nordic default; retained across HCI Reset"),
    Command(0xFD1B, "VS Scan Channel Map Set", COMPLETE,
            b"\xff\xff\xff\xff\xff",
            note="all primary advertising channels enabled"),
    Command(0xFD1C, "VS Scan Accept Extended Advertising Packets Set", COMPLETE,
            b"\x01", note="enabled/default state"),
    Command(0xFD1D, "VS Set Role Priority", COMPLETE,
            struct.pack("<BHB", 0x04, 0, 0xFF),
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="initiator-secondary role, reset-to-default priority"),
    Command(0xFD1E, "VS Set Event Start Task", COMPLETE,
            lambda ctx: struct.pack("<BHI", 0x03, ctx.handle, 0),
            needs=NEEDS_CONN,
            expect=(STATUS_UNKNOWN_CONNECTION, STATUS_COMMAND_DISALLOWED),
            note="connection event task address 0 disables the trigger"),
    Command(0xFD20, "VS Enable Periodic Advertising Event Counter Reports",
            COMPLETE, b"\x00", note="disabled state; no asynchronous report flood"),
)

COMMANDS = tuple(_catalog.COMMANDS) + _CORE_62 + _SDC_VS
BY_OPCODE = dict(_catalog.BY_OPCODE)
for _command in _CORE_62 + _SDC_VS:
    if _command.opcode in BY_OPCODE:
        raise AssertionError("opcode 0x%04X listed twice" % _command.opcode)
    BY_OPCODE[_command.opcode] = _command

TEARDOWN = _catalog.TEARDOWN

assert all(BY_OPCODE[_opcode].phase == PHASE_EXTENDED
           for _opcode in EXTENDED_PHASE_OPCODES)

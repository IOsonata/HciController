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

COMMANDS = tuple(_catalog.COMMANDS) + _CORE_62
BY_OPCODE = dict(_catalog.BY_OPCODE)
for _command in _CORE_62:
    if _command.opcode in BY_OPCODE:
        raise AssertionError("opcode 0x%04X listed twice" % _command.opcode)
    BY_OPCODE[_command.opcode] = _command

TEARDOWN = _catalog.TEARDOWN

assert all(BY_OPCODE[_opcode].phase == PHASE_EXTENDED
           for _opcode in EXTENDED_PHASE_OPCODES)

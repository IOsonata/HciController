#!/usr/bin/env python3
"""Command coverage with supplemental SDC-header opcodes included."""

import sys

import command_coverage_base as _base


_base_firmware_tables = _base.firmware_tables


def firmware_tables(root, values):
    firmware = _base_firmware_tables(root, values)

    supplemental_path = _base.os.path.join(root, "src", "hci_sdc.cpp")
    supplemental_names = _base.dispatch_names(
        supplemental_path,
        "static const HciCmdEntry_t s_HciSdcCompatCommands[] = {",
        r"\{\s*(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+)",
    )

    unresolved = sorted(set(name for name in supplemental_names
                            if name not in values))
    if unresolved:
        raise SystemExit(
            "these supplemental SDC command entries have no resolved opcode value:\n  "
            + "\n  ".join(unresolved)
        )

    for name in supplemental_names:
        opcode = values[name]
        if opcode in firmware:
            raise SystemExit(
                "opcode 0x%04X appears in multiple command sources (%s, %s)"
                % (opcode, firmware[opcode], name)
            )
        firmware[opcode] = name

    return firmware


_base.firmware_tables = firmware_tables
sys.exit(_base.main(sys.argv))

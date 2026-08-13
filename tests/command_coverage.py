#!/usr/bin/env python3
"""Command coverage with supplemental SDC-header opcodes included."""

import sys

import command_coverage_base as _base


def firmware_tables(root, values):
    vendor_path = _base.os.path.join(root, "src", "hci_sdc_nrfxlib.cpp")
    vendor_names = _base.dispatch_names(
        vendor_path,
        "static const HciCmdEntry_t s_HciSdcCommands[] = {",
        r"(?:HCI_SDC_ENTRY_\w*\s*\(\s*|\{\s*)"
        r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+|HCI_COUNTERS_OPCODE)",
    )

    supplemental_path = _base.os.path.join(root, "src", "hci_sdc.cpp")
    supplemental_names = _base.dispatch_names(
        supplemental_path,
        "static const HciCmdEntry_t s_HciSdcCompatCommands[] = {",
        r"\{\s*(HCI_SDC_(?:COMPAT|SUPP)_OPCODE_[A-Z0-9_]+|"
        r"SDC_HCI_OPCODE_CMD_[A-Z0-9_]+)",
    )

    names = vendor_names + supplemental_names
    unresolved = sorted(set(name for name in names if name not in values))
    if unresolved:
        raise SystemExit(
            "these dispatch table entries have no resolved opcode value:\n  "
            + "\n  ".join(unresolved)
        )

    firmware = {}
    for name in names:
        opcode = values[name]
        if opcode in firmware:
            raise SystemExit(
                "opcode 0x%04X appears in both dispatch sources (%s, %s)"
                % (opcode, firmware[opcode], name)
            )
        firmware[opcode] = name
    return firmware


_base.firmware_tables = firmware_tables
sys.exit(_base.main(sys.argv))

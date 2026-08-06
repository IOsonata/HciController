#!/usr/bin/env python3
"""Check that the Python tooling drives every opcode the firmware dispatches.

tests/hardware/hci_commands.py holds one entry per HCI command, with a
parameter block that can actually be sent. src/hci_sdc_nrfxlib.cpp holds the
dispatch table. Nothing kept the two in step, and for most of this branch
they were far apart: the firmware answered 126 opcodes and the tooling drove
29, so every command added after the ACL credit fix had only ever been seen
by a compiled stub.

This reports both directions. An opcode in the firmware and not in the table
is a command nothing will ever send at a radio. An opcode in the table and
not in the firmware is a test for something that was removed.

Opcode values come from the nrfxlib headers, because the dispatch table names
them symbolically, so this needs a checkout:

    python3 tests/command_coverage.py /path/to/sdk-nrfxlib
    NRFXLIB_DIR=/path/to/sdk-nrfxlib python3 tests/command_coverage.py

Exit status is 0 when the two agree, 1 when they do not, and 0 with a message
when there is no nrfxlib to resolve against.
"""

import glob
import os
import re
import sys


def repo_root():
    d = os.path.abspath(os.path.dirname(__file__))
    while True:
        if os.path.isfile(os.path.join(d, "include", "hci_h4.h")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def opcode_values(nrfxlib, root):
    """Every SDC_HCI_OPCODE_CMD_ name, plus this firmware's own vendor one."""
    values = {}
    pattern = os.path.join(nrfxlib, "softdevice_controller", "include", "*.h")
    for header in glob.glob(pattern):
        with open(header, "r") as handle:
            text = handle.read()
        for match in re.finditer(
                r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)",
                text):
            values[match.group(1)] = int(match.group(2), 16)

    counters = os.path.join(root, "include", "hci_counters.h")
    with open(counters, "r") as handle:
        match = re.search(r"#define\s+HCI_COUNTERS_OPCODE\s+(0x[0-9A-Fa-f]+)",
                          handle.read())
    if match:
        values["HCI_COUNTERS_OPCODE"] = int(match.group(1), 16)
    return values


def firmware_table(root, values):
    """The opcodes src/hci_sdc_nrfxlib.cpp dispatches, by name and number."""
    path = os.path.join(root, "src", "hci_sdc_nrfxlib.cpp")
    with open(path, "r") as handle:
        text = handle.read()

    start = text.find("static const HciCmdEntry_t s_HciSdcCommands[] = {")
    if start < 0:
        raise SystemExit("dispatch table not found in %s" % path)
    end = text.find("\n};", start)
    body = re.sub(r"/\*.*?\*/", "", text[start:end], flags=re.S)

    names = re.findall(
        r"(?:HCI_SDC_ENTRY_\w*\s*\(\s*|\{\s*)"
        r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+|HCI_COUNTERS_OPCODE)", body)

    unresolved = sorted(set(n for n in names if n not in values))
    if unresolved:
        raise SystemExit(
            "these dispatch table entries are not defined by this nrfxlib:\n  "
            + "\n  ".join(unresolved))

    return {values[n]: n for n in names}


def main(argv):
    root = repo_root()
    if root is None:
        print("HciController root not found")
        return 1

    nrfxlib = None
    if len(argv) > 1:
        nrfxlib = argv[1]
    elif os.environ.get("NRFXLIB_DIR"):
        nrfxlib = os.environ["NRFXLIB_DIR"]
    else:
        guess = os.path.join(root, "..", "external", "sdk-nrfxlib")
        if os.path.isdir(guess):
            nrfxlib = guess

    if not nrfxlib or not os.path.isdir(
            os.path.join(nrfxlib, "softdevice_controller", "include")):
        print("no sdk-nrfxlib, skipped. Pass one as an argument or set "
              "NRFXLIB_DIR.")
        return 0

    sys.path.insert(0, os.path.join(root, "tests", "hardware"))
    import hci_commands

    values = opcode_values(nrfxlib, root)
    firmware = firmware_table(root, values)
    tooling = dict(hci_commands.BY_OPCODE)

    undriven = sorted(set(firmware) - set(tooling))
    orphaned = sorted(set(tooling) - set(firmware))

    for opcode in undriven:
        print("[!!] 0x%04X %-56s dispatched, not driven"
              % (opcode, firmware[opcode]))
    for opcode in orphaned:
        print("[!!] 0x%04X %-56s driven, not dispatched"
              % (opcode, tooling[opcode].name))

    if undriven or orphaned:
        print("\n%d opcode(s) disagree between src/hci_sdc_nrfxlib.cpp and "
              "tests/hardware/hci_commands.py."
              % (len(undriven) + len(orphaned)))
        return 1

    # What the probe can send without help is worth stating, because the
    # difference between the table being complete and the radio having seen
    # every command is the part a reader will otherwise assume away.
    buckets = {}
    for command in hci_commands.BY_OPCODE.values():
        buckets[command.needs] = buckets.get(command.needs, 0) + 1

    print("[ok] %d opcodes, dispatched and driven, both ways."
          % len(firmware))
    print("     %d need nothing, %d need a connection, %d an advertising set,"
          % (buckets.get(hci_commands.NEEDS_NOTHING, 0),
             buckets.get(hci_commands.NEEDS_CONN, 0),
             buckets.get(hci_commands.NEEDS_ADV_SET, 0)))
    print("     %d a periodic sync, %d are only sent when asked."
          % (buckets.get(hci_commands.NEEDS_SYNC, 0),
             buckets.get(hci_commands.NEEDS_CONSENT, 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

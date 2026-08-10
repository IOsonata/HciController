#!/usr/bin/env python3
"""Check that the current Core profile drives every opcode it exposes.

The controller has two command dispatch sources:

    src/hci_sdc_nrfxlib.cpp   commands mapped directly to nrfxlib
    src/hci_sdc.cpp           compatibility commands supplied by this bridge

tests/hardware/hci_commands.py holds the broad radio probe table. Dedicated
profile probes may cover compatibility commands that need stronger semantic
checks than a generic command row; hci_core54_profile_test.py declares those
opcodes explicitly.

The current product profile can also hide backend commands from a newer Core
revision. The same profile test declares those excluded opcodes, so this check
compares the externally exposed HCI controller rather than every function the
linked nrfxlib happens to contain.

Opcode values for the vendor table come from nrfxlib headers:

    python3 tests/command_coverage.py /path/to/sdk-nrfxlib
    NRFXLIB_DIR=/path/to/sdk-nrfxlib python3 tests/command_coverage.py

Exit status is 0 when exposed firmware and hardware tooling agree, 1 when they
do not, and 0 with a message when there is no nrfxlib checkout to resolve the
vendor opcode names.
"""

import ast
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
    """Every vendor opcode name plus locally defined bridge opcodes."""
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

    compat = os.path.join(root, "src", "hci_sdc.cpp")
    with open(compat, "r") as handle:
        text = handle.read()
    for match in re.finditer(
            r"#define\s+(HCI_SDC_COMPAT_OPCODE_[A-Z0-9_]+)\s+"
            r"(0x[0-9A-Fa-f]+)U?", text):
        values[match.group(1)] = int(match.group(2), 16)

    return values


def dispatch_names(path, declaration, name_pattern):
    with open(path, "r") as handle:
        text = handle.read()

    start = text.find(declaration)
    if start < 0:
        raise SystemExit("dispatch table not found in %s" % path)
    end = text.find("\n};", start)
    if end < 0:
        raise SystemExit("dispatch table end not found in %s" % path)

    body = re.sub(r"/\*.*?\*/", "", text[start:end], flags=re.S)
    return re.findall(name_pattern, body)


def firmware_tables(root, values):
    """Externally reachable vendor and compatibility dispatcher opcodes."""
    vendor_path = os.path.join(root, "src", "hci_sdc_nrfxlib.cpp")
    vendor_names = dispatch_names(
        vendor_path,
        "static const HciCmdEntry_t s_HciSdcCommands[] = {",
        r"(?:HCI_SDC_ENTRY_\w*\s*\(\s*|\{\s*)"
        r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+|HCI_COUNTERS_OPCODE)",
    )

    compat_path = os.path.join(root, "src", "hci_sdc.cpp")
    compat_names = dispatch_names(
        compat_path,
        "static const HciCmdEntry_t s_HciSdcCompatCommands[] = {",
        r"\{\s*(HCI_SDC_COMPAT_OPCODE_[A-Z0-9_]+)",
    )

    names = vendor_names + compat_names
    unresolved = sorted(set(n for n in names if n not in values))
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


def literal_set(path, variable):
    """Read a literal set assignment without importing hardware/serial code."""
    with open(path, "r") as handle:
        tree = ast.parse(handle.read(), filename=path)

    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == variable
                   for target in node.targets):
            continue
        value = ast.literal_eval(node.value)
        if not isinstance(value, set):
            raise SystemExit("%s in %s is not a literal set" % (variable, path))
        return set(value)

    raise SystemExit("%s not found in %s" % (variable, path))


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

    profile_path = os.path.join(
        root, "tests", "hardware", "hci_core54_profile_test.py"
    )
    covered = literal_set(profile_path, "COVERED_OPCODES")
    excluded = literal_set(profile_path, "EXCLUDED_OPCODES")

    values = opcode_values(nrfxlib, root)
    firmware_all = firmware_tables(root, values)
    firmware = {
        opcode: name for opcode, name in firmware_all.items()
        if opcode not in excluded
    }

    tooling = set(hci_commands.BY_OPCODE) | covered
    tooling -= excluded

    undriven = sorted(set(firmware) - tooling)
    orphaned = sorted(tooling - set(firmware))

    for opcode in undriven:
        print("[!!] 0x%04X %-56s exposed, not driven"
              % (opcode, firmware[opcode]))
    for opcode in orphaned:
        if opcode in hci_commands.BY_OPCODE:
            name = hci_commands.BY_OPCODE[opcode].name
        else:
            name = "dedicated Core profile probe"
        print("[!!] 0x%04X %-56s driven, not exposed" % (opcode, name))

    if undriven or orphaned:
        print(
            "\n%d opcode(s) disagree between the exposed dispatch profile "
            "and hardware tooling."
            % (len(undriven) + len(orphaned))
        )
        return 1

    buckets = {}
    for opcode, command in hci_commands.BY_OPCODE.items():
        if opcode in excluded:
            continue
        for need in command.needs:
            buckets[need] = buckets.get(need, 0) + 1

    print("[ok] %d exposed opcodes, all driven." % len(firmware))
    print("     %d vendor/compat dispatch opcodes hidden by the Core profile."
          % len(set(firmware_all) & excluded))
    print("     %d compatibility opcodes have dedicated profile coverage."
          % len(covered))
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

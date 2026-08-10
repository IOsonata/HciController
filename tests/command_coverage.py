#!/usr/bin/env python3
"""Check that the current Core profile drives every opcode it exposes.

The controller has two command dispatch sources:

    src/hci_sdc_nrfxlib.cpp   commands mapped directly to nrfxlib
    src/hci_sdc.cpp           compatibility commands supplied by this bridge

tests/hardware/hci_commands.py holds the broad radio probe table. Dedicated
profile probes may cover compatibility commands that need stronger semantic
checks than a generic command row; hci_core54_profile_test.py declares those
opcodes explicitly.

This check also ties Core 5.4 product claims to the resource layer. In
particular, a full LE Supported States bitmap needs the multirole resources and
parallel scan/initiate capability which make those combinations possible, and
SCA Updates plus CIS support activates Core 5.4 C.44 (LE Request Peer SCA).
Those checks are derived from source/resource configuration rather than from
the dispatch table, so the implementation cannot prove itself complete merely
by keeping two copies of the same opcode list in agreement.

Opcode values for the vendor table come from nrfxlib headers:

    python3 tests/command_coverage.py /path/to/sdk-nrfxlib
    NRFXLIB_DIR=/path/to/sdk-nrfxlib python3 tests/command_coverage.py

Exit status is 0 when exposed firmware and hardware tooling agree, 1 when they
do not, and 0 with a message when there is no nrfxlib checkout to resolve the
vendor opcode names. Source/resource profile checks run even without nrfxlib.
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


def read_text(path):
    with open(path, "r") as handle:
        return handle.read()


def macro_int(text, name):
    match = re.search(
        r"^\s*#define\s+%s\s+([0-9]+|0x[0-9A-Fa-f]+)U?\s*$"
        % re.escape(name),
        text,
        flags=re.M,
    )
    if not match:
        raise SystemExit("cannot resolve integer macro %s" % name)
    return int(match.group(1), 0)


def validate_core54_source_contracts(root):
    """Independent source/resource checks behind exposed Core 5.4 claims."""
    resource_header = read_text(os.path.join(root, "include", "hci_sdc_resources.h"))
    resource_source = read_text(os.path.join(root, "src", "hci_sdc_resources.cpp"))
    compat_source = read_text(os.path.join(root, "src", "hci_sdc.cpp"))
    vendor_source = read_text(os.path.join(root, "src", "hci_sdc_nrfxlib.cpp"))
    app_source = read_text(os.path.join(root, "src", "hci_app.cpp"))

    peripheral = macro_int(resource_header, "HCI_SDC_PERIPHERAL_COUNT")
    central = macro_int(resource_header, "HCI_SDC_CENTRAL_COUNT")
    adv_sets = macro_int(resource_header, "HCI_SDC_ADV_SET_COUNT")
    scan_buffers = macro_int(resource_header, "HCI_SDC_SCAN_BUFFER_COUNT")

    # The compatibility implementation currently claims all legacy states
    # 0..41. That includes combinations needing multiple simultaneous links
    # in each role, advertising, scanning and scan+initiate concurrency.
    full_states = bool(re.search(
        r"static\s+const\s+uint8_t\s+states\s*\[8\]\s*=\s*\{\s*"
        r"0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,"
        r"\s*0x03U\s*,\s*0x00U\s*,\s*0x00U",
        compat_source,
        flags=re.S,
    ))

    if full_states:
        failures = []
        if peripheral < 2:
            failures.append("at least two Peripheral links")
        if central < 2:
            failures.append("at least two Central links")
        if adv_sets < 1:
            failures.append("an advertising set")
        if scan_buffers < 1:
            failures.append("a scan buffer")
        if "sdc_support_ext_adv();" not in resource_source:
            failures.append("extended/legacy advertising support")
        if "sdc_support_peripheral();" not in resource_source:
            failures.append("Peripheral role support")
        if "sdc_support_ext_central();" not in resource_source:
            failures.append("Central/scanning/initiating support")
        if "sdc_support_parallel_scanning_and_initiating();" not in resource_source:
            failures.append("parallel scanning and initiating support")

        if failures:
            raise SystemExit(
                "LE Read Supported States claims all legacy combinations but "
                "the resource profile is missing:\n  " + "\n  ".join(failures)
            )
        print("[ok] full LE Supported States claim is backed by multirole resources")

    # Core 5.4 C.44 is active when SCA Updates and either CIS role are enabled.
    # LE Request Peer SCA belongs to the nrfxlib vendor table; require its
    # actual table row here so Supported Commands and a handler declaration
    # alone cannot masquerade as an executable command.
    sca = (
        "sdc_support_sca_central();" in resource_source
        or "sdc_support_sca_peripheral();" in resource_source
    )
    cis = (
        "sdc_support_cis_central();" in resource_source
        or "sdc_support_cis_peripheral();" in resource_source
    )
    peer_sca_row = re.search(
        r"HCI_SDC_ENTRY_[A-Z]+\s*\(\s*"
        r"SDC_HCI_OPCODE_CMD_LE_REQUEST_PEER_SCA\s*,",
        vendor_source,
    )
    if sca and cis and not peer_sca_row:
        raise SystemExit(
            "Core 5.4 C.44 is active (SCA Updates + CIS) but LE Request Peer "
            "SCA is absent from the nrfxlib dispatch table"
        )
    if sca and cis:
        print("[ok] C.44 active resource profile has vendor LE Request Peer SCA routing")

    # HciTarget documents USB operations as optional. Selecting USB as the HCI
    # host must therefore be rejected before any optional target function is
    # dereferenced.
    if not re.search(
        r"HostType\s*==\s*HCI_APP_HOST_USB\s*&&\s*!HciTargetHasUsb\(&Target\)",
        app_source,
    ):
        raise SystemExit(
            "HciAppInit does not reject USB host selection on a target without "
            "USB operations"
        )
    print("[ok] USB host selection is guarded by HciTargetHasUsb")

    # A successful stop must shut the hardware down before releasing TinyUSB's
    # process-global owner/root-port state.
    stop_at = app_source.find("void HciAppStop(")
    if stop_at < 0:
        raise SystemExit("HciAppStop not found")
    stop_body = app_source[stop_at:]
    target_stop = stop_body.find("pApp->Target.pOps->Stop")
    usb_release = stop_body.find("HciAppUsbRelease(pApp)")
    if target_stop < 0 or usb_release < 0 or target_stop > usb_release:
        raise SystemExit(
            "HciAppStop must stop target USB hardware before releasing TinyUSB"
        )
    print("[ok] application stop orders target teardown before TinyUSB release")


def opcode_values(nrfxlib, root):
    """Every vendor opcode name plus locally defined bridge opcodes."""
    values = {}
    pattern = os.path.join(nrfxlib, "softdevice_controller", "include", "*.h")
    for header in glob.glob(pattern):
        text = read_text(header)
        for match in re.finditer(
                r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)",
                text):
            values[match.group(1)] = int(match.group(2), 16)

    counters = os.path.join(root, "include", "hci_counters.h")
    match = re.search(r"#define\s+HCI_COUNTERS_OPCODE\s+(0x[0-9A-Fa-f]+)",
                      read_text(counters))
    if match:
        values["HCI_COUNTERS_OPCODE"] = int(match.group(1), 16)

    compat_paths = (
        os.path.join(root, "include", "hci_sdc.h"),
        os.path.join(root, "src", "hci_sdc.cpp"),
    )
    compat_pattern = re.compile(
        r"#define\s+(HCI_SDC_COMPAT_OPCODE_[A-Z0-9_]+)\s+"
        r"(0x[0-9A-Fa-f]+)U?"
    )
    for compat in compat_paths:
        text = read_text(compat)
        for match in compat_pattern.finditer(text):
            name = match.group(1)
            value = int(match.group(2), 16)
            if name in values and values[name] != value:
                raise SystemExit(
                    "%s has conflicting opcode values 0x%04X and 0x%04X"
                    % (name, values[name], value)
                )
            values[name] = value

    return values


def dispatch_names(path, declaration, name_pattern):
    text = read_text(path)

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

    validate_core54_source_contracts(root)

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
        print("no sdk-nrfxlib, opcode-table comparison skipped. Pass one as an "
              "argument or set NRFXLIB_DIR.")
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

    # C.44 is derived from resources above, not from the opcode table. Once
    # active, the externally exposed profile must contain 0x206D even if both
    # the broad probe and dispatch table were accidentally edited together.
    resource_source = read_text(os.path.join(root, "src", "hci_sdc_resources.cpp"))
    sca = "sdc_support_sca_" in resource_source
    cis = "sdc_support_cis_" in resource_source
    if sca and cis and 0x206D not in firmware:
        print("[!!] 0x206D LE Request Peer SCA required by active C.44, not exposed")
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

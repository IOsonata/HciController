#!/usr/bin/env python3
"""Check that the nRF52840 + SDC HEAD profile drives every exposed opcode.

The controller has three command sources:

    src/hci_sdc_nrfxlib.cpp   commands mapped by the nrfxlib table
    src/hci_sdc.cpp           supplemental Core commands
    src/hci_controller.cpp    bridge-local vendor diagnostics

The official harness command catalog plus harness target-profile metadata must
account for every externally reachable opcode. Source/resource checks run
independently so keeping opcode lists in agreement cannot by itself prove the
product profile.
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


def validate_source_contracts(root):
    resource_header = read_text(os.path.join(root, "include", "hci_sdc_resources.h"))
    resource_source = read_text(os.path.join(root, "src", "hci_sdc_resources.cpp"))
    supplemental_source = read_text(os.path.join(root, "src", "hci_sdc.cpp"))
    vendor_source = read_text(os.path.join(root, "src", "hci_sdc_nrfxlib.cpp"))
    profile_header = read_text(os.path.join(root, "include", "hci_core_profile.h"))
    app_source = read_text(os.path.join(root, "src", "hci_app.cpp"))

    peripheral = macro_int(resource_header, "HCI_SDC_PERIPHERAL_COUNT")
    central = macro_int(resource_header, "HCI_SDC_CENTRAL_COUNT")
    adv_sets = macro_int(resource_header, "HCI_SDC_ADV_SET_COUNT")
    scan_buffers = macro_int(resource_header, "HCI_SDC_SCAN_BUFFER_COUNT")

    full_states = bool(re.search(
        r"static\s+const\s+uint8_t\s+states\s*\[8\]\s*=\s*\{\s*"
        r"0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,\s*0xFFU\s*,"
        r"\s*0x03U\s*,\s*0x00U\s*,\s*0x00U",
        supplemental_source,
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

    sca = "sdc_support_sca_" in resource_source
    cis = "sdc_support_cis_" in resource_source
    peer_sca_row = re.search(
        r"HCI_SDC_ENTRY_[A-Z]+\s*\(\s*"
        r"SDC_HCI_OPCODE_CMD_LE_REQUEST_PEER_SCA\s*,",
        vendor_source,
    )
    if sca and cis and not peer_sca_row:
        raise SystemExit(
            "SCA Updates + CIS are active but LE Request Peer SCA is absent "
            "from the nrfxlib dispatch table"
        )
    if sca and cis:
        print("[ok] SCA+CIS profile has vendor LE Request Peer SCA routing")

    target = re.search(
        r"#define\s+HCI_CONTROLLER_TARGET_CORE_VERSION\s+"
        r"HCI_CORE_VERSION_([0-9_]+)", profile_header
    )
    if target and target.group(1) == "6_2":
        required_calls = (
            "sdc_support_extended_feature_set_central();",
            "sdc_support_extended_feature_set_peripheral();",
            "sdc_support_frame_space_update_central();",
            "sdc_support_frame_space_update_peripheral();",
            "sdc_support_shorter_connection_intervals_central();",
            "sdc_support_shorter_connection_intervals_peripheral();",
        )
        missing = [call for call in required_calls if call not in resource_source]
        if missing:
            raise SystemExit(
                "Core 6.2 profile is missing SDC support calls:\n  "
                + "\n  ".join(missing)
            )
        for macro in (
            "HCI_SDC_MEM_FRAME_SPACE_UPDATE",
            "HCI_SDC_MEM_SHORTER_CONNECTION_INTERVALS",
        ):
            if macro not in resource_header:
                raise SystemExit("Core 6.2 resource term %s is missing" % macro)
        print("[ok] Core 6.2 profile has Extended Features, FSU and SCI resources")

    init_mode_at = app_source.find("bool HciAppInitMode(")
    init_compat_at = app_source.find("\nbool HciAppInit(", init_mode_at)
    if init_mode_at < 0 or init_compat_at < 0:
        raise SystemExit("HciAppInitMode not found")
    init_mode_body = app_source[init_mode_at:init_compat_at]
    if not re.search(
        r"hostType\s*==\s*HCI_APP_HOST_USB\s*&&\s*!HciTargetHasUsb\(&Target\)",
        init_mode_body,
    ):
        raise SystemExit(
            "HciAppInitMode does not reject USB host selection on a target without USB operations"
        )
    print("[ok] USB host selection is guarded by HciTargetHasUsb")

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

    controller_header = read_text(
        os.path.join(root, "include", "hci_controller.h")
    )
    for name in (
        "HCI_CONTROLLER_LOOPBACK_OPCODE",
        "HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE",
    ):
        values[name] = macro_int(controller_header, name)

    local_paths = (
        os.path.join(root, "include", "hci_sdc.h"),
        os.path.join(root, "src", "hci_sdc.cpp"),
    )
    local_pattern = re.compile(
        r"#define\s+(HCI_SDC_(?:COMPAT|SUPP)_OPCODE_[A-Z0-9_]+)\s+"
        r"(0x[0-9A-Fa-f]+)U?"
    )
    for path in local_paths:
        text = read_text(path)
        for match in local_pattern.finditer(text):
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
    vendor_path = os.path.join(root, "src", "hci_sdc_nrfxlib.cpp")
    vendor_names = dispatch_names(
        vendor_path,
        "static const HciCmdEntry_t s_HciSdcCommands[] = {",
        r"(?:HCI_SDC_ENTRY_\w*\s*\(\s*|\{\s*)"
        r"(SDC_HCI_OPCODE_CMD_[A-Z0-9_]+|HCI_COUNTERS_OPCODE)",
    )

    supplemental_path = os.path.join(root, "src", "hci_sdc.cpp")
    supplemental_names = dispatch_names(
        supplemental_path,
        "static const HciCmdEntry_t s_HciSdcCompatCommands[] = {",
        r"\{\s*(HCI_SDC_(?:COMPAT|SUPP)_OPCODE_[A-Z0-9_]+)",
    )

    controller_path = os.path.join(root, "src", "hci_controller.cpp")
    controller_source = read_text(controller_path)
    local_names = []
    for name in (
        "HCI_CONTROLLER_LOOPBACK_OPCODE",
        "HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE",
    ):
        if not re.search(
                r"if\s*\(\s*opcode\s*==\s*%s\s*\)" % re.escape(name),
                controller_source):
            raise SystemExit("local command %s is not routed by hci_controller.cpp" % name)
        local_names.append(name)

    names = vendor_names + supplemental_names + local_names
    unresolved = sorted(set(n for n in names if n not in values))
    if unresolved:
        raise SystemExit(
            "these command entries have no resolved opcode value:\n  "
            + "\n  ".join(unresolved)
        )

    firmware = {}
    for name in names:
        opcode = values[name]
        if opcode in firmware:
            raise SystemExit(
                "opcode 0x%04X appears in multiple command sources (%s, %s)"
                % (opcode, firmware[opcode], name)
            )
        firmware[opcode] = name
    return firmware


def literal_set(path, variable):
    with open(path, "r") as handle:
        tree = ast.parse(handle.read(), filename=path)

    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == variable
                   for target in node.targets):
            continue
        if (isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name)
                and node.value.func.id == "set"
                and not node.value.args and not node.value.keywords):
            return set()
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

    validate_source_contracts(root)

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
        print("no sdk-nrfxlib, opcode-table comparison skipped. Pass one as an argument or set NRFXLIB_DIR.")
        return 0

    harness_lib = os.path.join(root, "tests", "harness", "lib")
    sys.path.insert(0, harness_lib)
    import hci_commands

    profile_path = os.path.join(harness_lib, "target_profile.py")
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
        name = (hci_commands.BY_OPCODE[opcode].name
                if opcode in hci_commands.BY_OPCODE
                else "dedicated nRF52840 profile probe")
        print("[!!] 0x%04X %-56s driven, not exposed" % (opcode, name))

    if undriven or orphaned:
        print("\n%d opcode(s) disagree between the exposed command profile and harness tooling."
              % (len(undriven) + len(orphaned)))
        return 1

    resource_source = read_text(os.path.join(root, "src", "hci_sdc_resources.cpp"))
    if "sdc_support_sca_" in resource_source and "sdc_support_cis_" in resource_source:
        if 0x206D not in firmware:
            print("[!!] 0x206D LE Request Peer SCA required by active SCA+CIS profile, not exposed")
            return 1

    buckets = {}
    for opcode, command in hci_commands.BY_OPCODE.items():
        if opcode in excluded:
            continue
        for need in command.needs:
            buckets[need] = buckets.get(need, 0) + 1

    print("[ok] %d exposed opcodes, all driven." % len(firmware))
    print("     %d command opcodes hidden by the target profile."
          % len(set(firmware_all) & excluded))
    print("     %d supplemental opcodes have dedicated profile coverage."
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

#!/usr/bin/env python3
"""
Audit the SDC surface used by the nRF52840 HciController release.

This release intentionally follows sdk-nrfxlib HEAD. The test therefore runs
in both directions:

* every sdc_hci_cmd_* function called by HciController must exist in the nRF52
  multirole archive;
* every non-experimental SDC capability selected for release 1 must still be
  present in the HEAD archive/header and routed by HciController;
* every sdc_support_* API declared by HEAD must be used or explicitly
  classified for the nRF52840 release;
* nRF52840 hardware exclusions must never be enabled accidentally.

    python3 tests/sdc_symbols.py
    python3 tests/sdc_symbols.py /path/to/libsoftdevice_controller_multirole.a

The archive symbol index is read directly, so no Arm toolchain is required.
"""

import os
import re
import struct
import sys

# The nRF52 multirole archive does not export this command. HciController
# supplies 0x201C in src/hci_sdc.cpp instead.
KNOWN_ABSENT = ["sdc_hci_cmd_le_read_supported_states"]

DEFAULT_LIB = ("../external/sdk-nrfxlib/softdevice_controller/lib/nrf52/"
               "hard-float/libsoftdevice_controller_multirole.a")

TABLE_SOURCES = (
    ("src", "hci_sdc_nrfxlib.cpp"),
    ("src", "hci_sdc.cpp"),
)
RESOURCE_SOURCE = ("src", "hci_sdc_resources.cpp")

# Core 6.2 HCI entry points that current nRF52 multirole SDC must provide and
# HciController must route. These are release requirements now, not candidates.
REQUIRED_HCI = {
    "sdc_hci_cmd_le_frame_space_update",
    "sdc_hci_cmd_le_conn_rate_request",
    "sdc_hci_cmd_le_set_default_rate_params",
    "sdc_hci_cmd_le_read_min_supported_conn_interval",
}

# Support APIs that must be present in the configured release-1 profile.
REQUIRED_SUPPORT = {
    "sdc_support_ext_adv",
    "sdc_support_peripheral",
    "sdc_support_ext_central",
    "sdc_support_parallel_scanning_and_initiating",
    "sdc_support_le_2m_phy",
    "sdc_support_le_coded_phy",
    "sdc_support_dle_central",
    "sdc_support_dle_peripheral",
    "sdc_support_phy_update_central",
    "sdc_support_phy_update_peripheral",
    "sdc_support_direct_test_mode",
    "sdc_support_le_privacy",
    "sdc_support_qos_channel_survey",
    "sdc_support_le_power_control_central",
    "sdc_support_le_power_control_peripheral",
    "sdc_support_le_path_loss_monitoring",
    "sdc_support_sca_central",
    "sdc_support_sca_peripheral",
    "sdc_support_connection_subrating_central",
    "sdc_support_connection_subrating_peripheral",
    "sdc_support_extended_feature_set_central",
    "sdc_support_extended_feature_set_peripheral",
    "sdc_support_frame_space_update_central",
    "sdc_support_frame_space_update_peripheral",
    "sdc_support_shorter_connection_intervals_central",
    "sdc_support_shorter_connection_intervals_peripheral",
    "sdc_support_le_periodic_adv",
    "sdc_support_le_periodic_sync",
    "sdc_support_periodic_adv_sync_transfer_sender_central",
    "sdc_support_periodic_adv_sync_transfer_sender_peripheral",
    "sdc_support_periodic_adv_sync_transfer_receiver_central",
    "sdc_support_periodic_adv_sync_transfer_receiver_peripheral",
    "sdc_support_le_periodic_adv_with_rsp",
    "sdc_support_le_periodic_sync_with_rsp",
    "sdc_support_cis_central",
    "sdc_support_cis_peripheral",
    "sdc_support_bis_source",
    "sdc_support_bis_sink",
}

# Deliberate nRF52840 exclusions. These must never be called by release 1 even
# though the shared SDC header declares them for other Nordic families.
NRF52840_EXCLUDED = {
    # nRF52840 has no Bluetooth Direction Finding radio support.
    "sdc_support_le_conn_cte_rsp_central",
    "sdc_support_le_conn_cte_rsp_peripheral",
    "sdc_support_le_connectionless_cte_transmitter",
    # sdk-nrfxlib documents Channel Sounding as unavailable on nRF52/nRF53.
    "sdc_support_channel_sounding_test",
    "sdc_support_channel_sounding_mode3",
    "sdc_support_channel_sounding_initiator_role",
    "sdc_support_channel_sounding_initiator_role_central",
    "sdc_support_channel_sounding_initiator_role_peripheral",
    "sdc_support_channel_sounding_reflector_role",
    "sdc_support_channel_sounding_reflector_role_central",
    "sdc_support_channel_sounding_reflector_role_peripheral",
}

# APIs intentionally not part of the release capability claim.
OPTIONAL_OR_ALTERNATIVE = {
    # Narrower role variants superseded by the selected multirole calls.
    "sdc_support_adv",
    "sdc_support_scan",
    "sdc_support_ext_scan",
    "sdc_support_central",
    # Adds only the Power Class 1 feature bit; board/RF design decides this.
    "sdc_support_le_power_class_1",
    # Optional FSU tuning; forces equal TX/RX PHYs on ACL connections.
    "sdc_support_lowest_frame_space",
    # Deprecated alias; role-specific calls are used instead.
    "sdc_support_extended_feature_set",
    # Explicitly experimental in sdk-nrfxlib HEAD.
    "sdc_support_flushable_acl_data",
    # Board/platform integrations, not Bluetooth HCI feature capability.
    "sdc_support_mpsl_coex",
    "sdc_support_mpsl_fem",
}


def archive_symbols(path):
    """Names in the GNU ar archive symbol index."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:8] != b"!<arch>\n":
        raise ValueError("%s is not an ar archive" % path)

    name = data[8:24].decode("ascii", "replace").strip()
    if name.startswith("__.SYMDEF"):
        raise ValueError("%s is a BSD archive; use nm on it instead" % path)
    if name != "/":
        raise ValueError("%s has no GNU symbol index" % path)

    size = int(data[56:66].decode("ascii").strip())
    body = data[68:68 + size]
    count = struct.unpack(">I", body[:4])[0]
    names = body[4 + count * 4:].split(b"\x00")
    return set(n.decode("ascii", "replace") for n in names if n)


def strip_noise(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#\s*include[^\n]*", " ", text, flags=re.M)
    return text


def source_hci_calls(paths):
    found = set()
    for path in paths:
        with open(path) as handle:
            text = strip_noise(handle.read())
        for name in re.findall(r"\b(sdc_hci_cmd_[a-z0-9_]+)\s*[(,)]", text):
            if not name.endswith("_t"):
                found.add(name)
    return set(found)


def support_declarations(path):
    with open(path) as handle:
        text = strip_noise(handle.read())
    return set(re.findall(r"\bvoid\s+(sdc_support_[a-z0-9_]+)\s*\(", text))


def support_calls(path):
    with open(path) as handle:
        text = strip_noise(handle.read())
    return set(re.findall(r"\b(sdc_support_[a-z0-9_]+)\s*\(", text))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    if len(sys.argv) > 2:
        print(__doc__.strip())
        return 2

    lib = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, DEFAULT_LIB)
    lib = os.path.normpath(lib)

    if not os.path.exists(lib):
        print("No library at %s" % lib)
        print("Pass the path to a libsoftdevice_controller_multirole.a.")
        return 2

    sdc_root = os.path.normpath(os.path.join(os.path.dirname(lib), "..", "..", ".."))
    sdc_h = os.path.join(sdc_root, "include", "sdc.h")
    if not os.path.exists(sdc_h):
        print("No SDC header at %s" % sdc_h)
        return 2

    table_paths = [os.path.join(root, *parts) for parts in TABLE_SOURCES]
    resource_path = os.path.join(root, *RESOURCE_SOURCE)
    for path in table_paths + [resource_path]:
        if not os.path.exists(path):
            print("No source at %s" % path)
            return 2

    try:
        symbols = archive_symbols(lib)
    except ValueError as error:
        print(error)
        return 2

    calls = source_hci_calls(table_paths)
    offered = sorted(s for s in symbols if s.startswith("sdc_hci_cmd_"))
    missing_calls = sorted(c for c in calls
                           if c not in symbols and c not in KNOWN_ABSENT)
    required_hci_missing = sorted(REQUIRED_HCI - symbols)
    required_hci_unrouted = sorted(REQUIRED_HCI - calls)

    print("%s" % lib)
    print("%d symbols, %d of them HCI commands" % (len(symbols), len(offered)))
    print("HciController calls %d SDC HCI entry points" % len(calls))
    print()

    for name in KNOWN_ABSENT:
        if name in symbols:
            print("  %-56s now present; remove the compatibility copy" % name)
        else:
            print("  %-56s absent, compatibility copy required" % name)

    if missing_calls:
        print()
        for name in missing_calls:
            print("  MISSING  %s" % name)
        print("\n%d SDC call(s) used by HciController are absent from this archive."
              % len(missing_calls))

    if required_hci_missing:
        print("\nRequired nRF52840 release HCI entry points disappeared from HEAD:")
        for name in required_hci_missing:
            print("  MISSING  %s" % name)

    if required_hci_unrouted:
        print("\nRequired nRF52840 release HCI entry points are not routed:")
        for name in required_hci_unrouted:
            print("  UNROUTED %s" % name)

    if missing_calls or required_hci_missing or required_hci_unrouted:
        return 1

    print("Every required SDC HCI function is present and routed.")

    declared = support_declarations(sdc_h)
    used = support_calls(resource_path)
    known = REQUIRED_SUPPORT | NRF52840_EXCLUDED | OPTIONAL_OR_ALTERNATIVE
    unknown = sorted(declared - known)
    impossible = sorted(used & NRF52840_EXCLUDED)
    required_not_declared = sorted(REQUIRED_SUPPORT - declared)
    required_not_used = sorted(REQUIRED_SUPPORT - used)

    print()
    print("SDC support API audit: %d declared, %d called by release 1"
          % (len(declared), len(used)))

    if impossible:
        print("\nHardware-excluded nRF52840 capabilities are being enabled:")
        for name in impossible:
            print("  INVALID  %s" % name)

    if required_not_declared:
        print("\nRequired release-1 SDC support APIs disappeared from HEAD:")
        for name in required_not_declared:
            print("  MISSING  %s" % name)

    if required_not_used:
        print("\nRequired release-1 SDC support calls are missing:")
        for name in required_not_used:
            print("  MISSING  %s" % name)

    if unknown:
        print("\nUnclassified sdc_support_* APIs appeared in sdk-nrfxlib HEAD:")
        for name in unknown:
            print("  NEW      %s" % name)
        print("Classify each new API before accepting the new HEAD as a release baseline.")

    if impossible or required_not_declared or required_not_used or unknown:
        return 1

    print("Every required nRF52840 SDC support capability is present and enabled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

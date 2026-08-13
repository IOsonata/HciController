#!/usr/bin/env python3
"""
Audit the complete SDC HCI command surface against HciController routing.

Every sdc_hci_cmd_* entry point exported by the selected nRF52 multirole SDC
archive must be either routed by HciController or explicitly classified below.
This is intentionally stricter than the release-profile audit.

    python3 tests/sdc_hci_surface.py
    python3 tests/sdc_hci_surface.py /path/to/libsoftdevice_controller_multirole.a

Do not add a classification merely to make this audit pass. Each entry must be
verified against the SDC headers/documentation and the nRF52840 capability set.
"""

import os
import sys

from sdc_symbols_release import (
    DEFAULT_LIB,
    KNOWN_ABSENT,
    TABLE_SOURCES,
    archive_symbols,
    source_hci_calls,
)

# Exported SDC HCI entry points intentionally not routed by HciController.
# An exported symbol alone is not proof that the configured controller can use
# the command. Keep each exclusion tied to its actual configuration or hardware
# limitation so a future nrfxlib update can re-evaluate it.
HCI_CLASSIFIED_NOT_ROUTED = {
    # Nordic exposes these only with LE Flushable ACL Data support. That feature
    # is experimental and this release intentionally does not call
    # sdc_support_flushable_acl_data(), so the commands are not advertised.
    "sdc_hci_cmd_cb_read_automatic_flush_timeout":
        "LE Flushable ACL Data is experimental and not enabled by this release",
    "sdc_hci_cmd_cb_write_automatic_flush_timeout":
        "LE Flushable ACL Data is experimental and not enabled by this release",

    # These symbols exist in the archive, but hardware evidence says the
    # Controller-to-Host flow-control group is unusable in this nRF52840
    # multirole configuration. Host Buffer Size returned 0x11; keep all three
    # hidden until the complete group is proven usable on hardware.
    "sdc_hci_cmd_cb_set_controller_to_host_flow_control":
        "exported SDC symbol but unusable on the tested nRF52840 multirole configuration",
    "sdc_hci_cmd_cb_host_buffer_size":
        "exported SDC symbol; Host Buffer Size returned 0x11 on tested nRF52840 multirole hardware",
    "sdc_hci_cmd_cb_host_number_of_completed_packets":
        "exported SDC symbol but unusable on the tested nRF52840 multirole configuration",

    # nRF52840 has no Bluetooth Direction Finding radio support.
    "sdc_hci_cmd_le_conn_cte_response_enable":
        "nRF52840 has no Direction Finding radio support; connection CTE response support is excluded",
    "sdc_hci_cmd_le_read_antenna_information":
        "nRF52840 has no Direction Finding radio support or antenna-switching capability exposed by SDC",
    "sdc_hci_cmd_le_set_conn_cte_transmit_params":
        "nRF52840 has no Direction Finding radio support; connection CTE transmit parameters are unusable",
    "sdc_hci_cmd_le_set_connless_cte_transmit_enable":
        "nRF52840 has no Direction Finding radio support; connectionless CTE transmitter support is excluded",
    "sdc_hci_cmd_le_set_connless_cte_transmit_params":
        "nRF52840 has no Direction Finding radio support; connectionless CTE transmit parameters are unusable",
}


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

    table_paths = [os.path.join(root, *parts) for parts in TABLE_SOURCES]
    for path in table_paths:
        if not os.path.exists(path):
            print("No source at %s" % path)
            return 2

    try:
        symbols = archive_symbols(lib)
    except ValueError as error:
        print(error)
        return 2

    offered = {s for s in symbols if s.startswith("sdc_hci_cmd_")}
    calls = source_hci_calls(table_paths)
    classified = set(HCI_CLASSIFIED_NOT_ROUTED)

    missing_calls = sorted(c for c in calls
                           if c not in symbols and c not in KNOWN_ABSENT)
    unclassified = sorted(offered - calls - classified)
    stale = sorted(classified - offered)
    classified_but_routed = sorted(classified & calls)
    empty_reasons = sorted(name for name, reason in HCI_CLASSIFIED_NOT_ROUTED.items()
                           if not reason.strip())

    print("%s" % lib)
    print("SDC exports %d HCI command entry points" % len(offered))
    print("HciController routes %d SDC HCI entry points" % len(calls))
    print("Explicitly classified but unrouted: %d" % len(classified))
    print()

    if missing_calls:
        print("HciController calls absent from this SDC archive:")
        for name in missing_calls:
            print("  MISSING      %s" % name)
        print()

    if unclassified:
        print("Exported SDC HCI entry points not routed or classified:")
        for name in unclassified:
            print("  UNCLASSIFIED %s" % name)
        print()

    if stale:
        print("Classifications that no longer correspond to SDC exports:")
        for name in stale:
            print("  STALE        %s" % name)
        print()

    if classified_but_routed:
        print("Classifications that are now routed and must be removed:")
        for name in classified_but_routed:
            print("  ROUTED       %s" % name)
        print()

    if empty_reasons:
        print("Classifications with no reason:")
        for name in empty_reasons:
            print("  NO-REASON    %s" % name)
        print()

    if missing_calls or unclassified or stale or classified_but_routed or empty_reasons:
        return 1

    print("Every exported SDC HCI command is routed or explicitly classified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

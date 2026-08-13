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
# Keep this empty until each candidate is individually verified. Values must
# state the concrete reason (alternate/deprecated API, unavailable hardware,
# unusable configuration, or an intentional optional exclusion).
HCI_CLASSIFIED_NOT_ROUTED = {
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

    # KNOWN_ABSENT commands are locally implemented compatibility commands and
    # therefore are not expected to appear in the archive's offered set.
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

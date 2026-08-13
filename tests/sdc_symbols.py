#!/usr/bin/env python3
"""Run both SDC release-profile and complete HCI surface audits."""

import sys

import sdc_hci_surface
import sdc_symbols_release


def main():
    release_rc = sdc_symbols_release.main()
    print()
    surface_rc = sdc_hci_surface.main()
    return 0 if release_rc == 0 and surface_rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""nRF52840 HciController release-profile coverage metadata."""

# These opcodes are exercised by dedicated profile/capability phases rather
# than the broad command catalog. command_coverage.py reads these as literals.
COVERED_OPCODES = {0x0C15, 0x0C16, 0x201C}

# 0xFFF2 is a legacy native-USB Event-IN diagnostic hook. The IOsonata UsbdHci
# product image does not provide that transport-specific snapshot, so the weak
# bridge fallback correctly answers Unknown HCI Command and the release profile
# must not advertise or probe it.
EXCLUDED_OPCODES = {0xFFF2}

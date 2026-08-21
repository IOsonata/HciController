#!/usr/bin/env python3
"""nRF52840 HciController release-profile coverage metadata."""

# These opcodes are exercised by dedicated profile/capability phases rather
# than the broad command catalog. command_coverage.py reads these as literals.
COVERED_OPCODES = {0x0C15, 0x0C16, 0x201C}
EXCLUDED_OPCODES = set()

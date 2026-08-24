#!/usr/bin/env python3
"""Run the periodic failure diagnostic over Bluetooth USB Bulk Serialization."""

import sys

import _bootstrap  # noqa: F401
# Importing the safe wrapper installs its pre-on_event semantic validation.
import periodic_failure_diag_safe  # noqa: F401
import periodic_failure_diag as diag
from hcicontroller.pair_transport import resolve_pair as _resolve_pair


def _resolve_bulk_pair(first=None, second=None, kind="auto", **kwargs):
    return _resolve_pair(
        first,
        second,
        kind=kind,
        bulk_serialization=True,
        **kwargs,
    )


# Keep the diagnostic itself unchanged; only select USB HCI alternate 1.
diag.resolve_pair = _resolve_bulk_pair


if __name__ == "__main__":
    sys.exit(diag.main())

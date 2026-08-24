#!/usr/bin/env python3
"""Run the existing two-controller CIS/ISO test over native Bluetooth USB HCI.

Both controllers are opened in USB Bulk Serialization mode because HCI ISO is
carried on the Bluetooth bulk endpoints in that alternate setting. The CIS test
logic itself remains in hci_cis_pair_test.py.
"""

import argparse
import sys

import hci_ble_test
import hci_cis_pair_test as cis
import hci_transport


class NativeIsoHci(cis.IsoHci):
    """Reuse the CIS test helper but read packets through the generic transport."""

    def read_wire(self, timeout=1.0):
        return hci_ble_test.Hci.read_wire(self, timeout)


def _bulk_spec(spec):
    return hci_transport.TransportSpec(
        "usb",
        spec.target,
        "%s bulk serialization" % spec,
        bulk_serialization=True,
    )


def _serial_number(spec):
    return str(getattr(spec.target, "serial_number", "") or "")


def _select(specs, selector, role):
    if selector is None:
        return None

    matches = [spec for spec in specs if _serial_number(spec) == selector]
    if not matches:
        raise cis.HciError("%s native USB controller %s was not found"
                           % (role, selector))
    if len(matches) != 1:
        raise cis.HciError("%s USB serial %s matched %d controllers"
                           % (role, selector, len(matches)))
    return matches[0]


def _controllers(args):
    try:
        specs = [_bulk_spec(spec) for spec in hci_transport.usb_candidates()]
    except hci_transport.SelectionError as err:
        raise cis.HciError(str(err))

    central = _select(specs, args.central, "central")
    peripheral = _select(specs, args.peripheral, "peripheral")

    if central is None and peripheral is None:
        if len(specs) != 2:
            labels = ", ".join(str(spec) for spec in specs) if specs else "none"
            raise cis.HciError("need exactly two native USB HCI controllers; detected %s"
                               % labels)
        return specs[0], specs[1]

    remaining = [spec for spec in specs
                 if spec is not central and spec is not peripheral]
    if central is None:
        if len(remaining) != 1:
            raise cis.HciError("need exactly one controller besides the peripheral")
        central = remaining[0]
    elif peripheral is None:
        if len(remaining) != 1:
            raise cis.HciError("need exactly one controller besides the central")
        peripheral = remaining[0]

    if central is peripheral:
        raise cis.HciError("central and peripheral controllers must differ")
    return central, peripheral


def main():
    parser = argparse.ArgumentParser(
        description="Run the CIS/ISO pair test over two native USB controllers")
    parser.add_argument("--central", metavar="USB-SERIAL",
                        help="central controller USB serial number")
    parser.add_argument("--peripheral", metavar="USB-SERIAL",
                        help="peripheral controller USB serial number")
    parser.add_argument("--raw", action="store_true", help="show raw HCI traffic")
    args = parser.parse_args()

    try:
        central, peripheral = _controllers(args)
    except cis.HciError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    old_iso_hci = cis.IsoHci
    old_find = cis.find_hci_ports
    old_argv = sys.argv
    cis.IsoHci = NativeIsoHci
    cis.find_hci_ports = lambda: [central, peripheral]
    sys.argv = [old_argv[0]] + (["--raw"] if args.raw else [])
    try:
        return cis.main()
    finally:
        sys.argv = old_argv
        cis.find_hci_ports = old_find
        cis.IsoHci = old_iso_hci


if __name__ == "__main__":
    sys.exit(main())

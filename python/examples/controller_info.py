#!/usr/bin/env python3
"""Open one HciController and print its HCI identity."""

import argparse
import sys

from hcicontroller import Hci, SelectionError, TransportSpec, addr_str, discover


def main():
    parser = argparse.ArgumentParser(
        description="Open one HciController and print its controller identity"
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "serial", "usb"),
        default="auto",
        help="transport discovery mode; auto prefers native USB",
    )
    parser.add_argument(
        "--usb",
        metavar="SERIAL_OR_VID:PID",
        help="select one native USB controller by serial number or VID:PID",
    )
    parser.add_argument(
        "--port",
        help="open this serial H:4 device instead of discovery",
    )
    parser.add_argument("--raw", action="store_true", help="show raw HCI traffic")
    args = parser.parse_args()

    if args.usb and args.port:
        parser.error("--usb and --port are mutually exclusive")

    try:
        if args.port:
            spec = TransportSpec(
                "serial", args.port, "serial H:4 %s" % args.port
            )
        else:
            kind = "usb" if args.usb else args.transport
            spec = discover(kind=kind, usb_selector=args.usb)
            if spec is None:
                raise SelectionError("no HciController found")
    except SelectionError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    hci = Hci(spec, raw=args.raw)
    try:
        hci.setup()
        identity, address_type, source = hci.identity()
        print("Controller: %s" % spec)
        print("Identity:   %s" % addr_str(identity))
        print("Type:       %s" % (
            "static random" if address_type == 0x01 else "public"
        ))
        print("Source:     %s" % source)
    finally:
        hci.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

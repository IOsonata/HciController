#!/usr/bin/env python3
"""Two-controller LE power-control hardware test."""

import argparse
import struct
import sys
import time

from hci_ble_test import (
    ERROR_NAMES,
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    H4_EVENT,
    HciError,
    HciGone,
    OP_DISCONNECT,
    OP_LE_SET_ADV_ENABLE,
    OP_LE_SET_EVENT_MASK,
    OP_LE_SET_RANDOM_ADDRESS,
    addr_str,
)
from hci_cis_pair_test import (
    IsoHci,
    find_hci_ports,
    start_advertising,
    start_connection,
    wait_acl_pair,
)

OP_LE_SET_PHY = 0x2032
OP_LE_READ_REMOTE_TX_POWER = 0x2077
OP_VS_WRITE_REMOTE_TX_POWER = 0xFD0A

LE_PHY_UPDATE_COMPLETE = 0x0C
LE_TRANSMIT_POWER_REPORTING = 0x21
POWER_REPORT_REASON_READ_REMOTE = 0x02

STATUS_CONTROLLER_BUSY = 0x3A
PHY_1M = 0x01
LE_EVENT_MASK = bytes.fromhex("ffffffffffffff1f")


def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))


def prepare(hci):
    hci.setup()
    hci.command(OP_LE_SET_EVENT_MASK, LE_EVENT_MASK)
    identity, addr_type, source = hci.identity()
    if addr_type == 0x01:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    return identity, addr_type, source


def read_pair_until(central, peripheral, predicate, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for label, hci in (("central", central), ("peripheral", peripheral)):
            packet = hci.read_packet(0.05)
            if packet is None:
                continue
            result = predicate(label, packet)
            if result is not None:
                return result
    raise HciError("timed out waiting for %s" % what)


def wait_phy_update(central, peripheral, handle, timeout=5.0):
    def match(label, packet):
        if label != "central":
            return None
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META or len(body) < 6:
            return None
        if body[0] != LE_PHY_UPDATE_COMPLETE:
            return None
        event_handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        if event_handle != handle:
            return None
        return body[1], body[4], body[5]

    status, tx_phy, rx_phy = read_pair_until(
        central, peripheral, match, timeout, "LE PHY Update Complete"
    )
    if status != 0:
        raise HciError("LE PHY Update Complete returned %s"
                       % status_text(status))
    print("Central PHY update complete: TX 0x%02X RX 0x%02X"
          % (tx_phy, rx_phy))


def wait_remote_power_report(central, peripheral, handle, timeout=5.0):
    def match(label, packet):
        if label != "central":
            return None
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META or len(body) < 9:
            return None
        if body[0] != LE_TRANSMIT_POWER_REPORTING:
            return None
        event_handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        if event_handle != handle:
            return None
        reason = body[4]
        if reason != POWER_REPORT_REASON_READ_REMOTE:
            print("Central power report reason 0x%02X ignored" % reason)
            return None
        tx_power = struct.unpack("<b", body[6:7])[0]
        delta = struct.unpack("<b", body[8:9])[0]
        return body[1], body[5], tx_power, body[7], delta

    status, phy, tx_power, flags, delta = read_pair_until(
        central, peripheral, match, timeout,
        "LE Transmit Power Reporting reason 0x02"
    )
    if status != 0:
        raise HciError("remote transmit power read completed with %s"
                       % status_text(status))
    print("Central remote TX power: PHY 0x%02X level %d dBm flags 0x%02X delta %d"
          % (phy, tx_power, flags, delta))


def read_remote_power(central, peripheral, handle):
    status, _ = central.command(
        OP_LE_READ_REMOTE_TX_POWER,
        struct.pack("<HB", handle, PHY_1M),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Read Remote Transmit Power Level returned %s"
                       % status_text(status))
    print("Central LE Read Remote Transmit Power Level accepted")
    wait_remote_power_report(central, peripheral, handle)


def verify_vendor_request_finished(central, peripheral, handle, timeout=5.0):
    deadline = time.time() + timeout
    busy = 0
    while time.time() < deadline:
        status, _ = central.command(
            OP_LE_READ_REMOTE_TX_POWER,
            struct.pack("<HB", handle, PHY_1M),
            allow_fail=True,
        )
        if status == STATUS_CONTROLLER_BUSY:
            busy += 1
            time.sleep(0.05)
            continue
        if status != 0:
            raise HciError(
                "post-0xFD0A LE Read Remote Transmit Power Level returned %s"
                % status_text(status)
            )
        if busy:
            print("0xFD0A procedure cleared after %d busy poll(s)" % busy)
        wait_remote_power_report(central, peripheral, handle)
        return
    raise HciError("0xFD0A left LE Power Control busy for %.1f seconds"
                   % timeout)


def cleanup(central, peripheral, central_acl, peripheral_acl):
    for hci, handle in ((central, central_acl), (peripheral, peripheral_acl)):
        if handle is None:
            continue
        try:
            hci.command(
                OP_DISCONNECT,
                struct.pack("<HB", handle, 0x13),
                allow_fail=True,
            )
        except (HciError, HciGone):
            pass

    try:
        peripheral.command(OP_LE_SET_ADV_ENABLE, b"\x00", allow_fail=True)
    except (HciError, HciGone):
        pass

    end = time.time() + 0.5
    while time.time() < end:
        for hci in (central, peripheral):
            try:
                hci.read_packet(0.02)
            except (HciError, HciGone):
                pass


def main():
    parser = argparse.ArgumentParser(
        description="Exercise LE power control between two HciControllers"
    )
    parser.add_argument("--central", help="central HCI serial port")
    parser.add_argument("--peripheral", help="peripheral HCI serial port")
    parser.add_argument("--raw", action="store_true", help="show raw H:4 traffic")
    args = parser.parse_args()

    ports = find_hci_ports()
    central_port = args.central
    peripheral_port = args.peripheral
    if central_port is None or peripheral_port is None:
        if len(ports) == 2:
            central_port = central_port or ports[0]
            peripheral_port = peripheral_port or ports[1]
        else:
            print("Need two HCI serial ports. Detected: %s"
                  % (", ".join(ports) if ports else "none"), file=sys.stderr)
            print("Use --central PORT --peripheral PORT.", file=sys.stderr)
            return 2

    if central_port == peripheral_port:
        print("Central and peripheral ports must differ.", file=sys.stderr)
        return 2

    print("LE power-control pair test")
    print("   central    %s" % central_port)
    print("   peripheral %s" % peripheral_port)

    central = IsoHci(central_port, raw=args.raw)
    peripheral = IsoHci(peripheral_port, raw=args.raw)
    central_acl = None
    peripheral_acl = None

    try:
        peripheral_id, peripheral_type, peripheral_source = prepare(peripheral)
        central_id, central_type, central_source = prepare(central)

        print("Peripheral identity %s (%s)"
              % (addr_str(peripheral_id), peripheral_source))
        print("Central identity    %s (%s)"
              % (addr_str(central_id), central_source))

        start_advertising(peripheral, peripheral_id, peripheral_type)
        start_connection(central, central_type, peripheral_id, peripheral_type)
        central_acl, peripheral_acl = wait_acl_pair(central, peripheral)

        # Reproduce the probe's Set PHY request, but do not start the next LL
        # procedure until its mandatory terminal event has arrived.
        status, _ = central.command(
            OP_LE_SET_PHY,
            struct.pack("<HBBBH", central_acl, 0, 0x07, 0x07, 0),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Set PHY returned %s" % status_text(status))
        print("Central LE Set PHY accepted")
        wait_phy_update(central, peripheral, central_acl)

        read_remote_power(central, peripheral, central_acl)

        status, _ = central.command(
            OP_VS_WRITE_REMOTE_TX_POWER,
            struct.pack("<HBb", central_acl, PHY_1M, 0),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("VS Write Remote TX Power returned %s"
                           % status_text(status))
        print("Central VS Write Remote TX Power accepted (1M, delta 0)")

        # Nordic documents no mandatory terminal event when delta=0 causes no
        # peer power change. A second standard read is therefore the observable
        # proof that the vendor procedure completed and released power control.
        verify_vendor_request_finished(central, peripheral, central_acl)

        print()
        print("PASS: central Set PHY completed before LE power control;")
        print("remote TX power read, VS Write Remote TX Power, and the")
        print("following power-control procedure all completed over the air.")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        cleanup(central, peripheral, central_acl, peripheral_acl)
        central.close()
        peripheral.close()


if __name__ == "__main__":
    sys.exit(main())

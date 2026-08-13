#!/usr/bin/env python3
"""Two-controller sweep of connection-scoped HCI catalog commands."""

import argparse
import struct
import sys
from types import SimpleNamespace

import hci_commands
from hci_ble_test import (
    HciError, HciGone, OP_DISCONNECT, OP_LE_EXT_CREATE_CONNECTION,
    OP_LE_SET_ADV_ENABLE, OP_LE_SET_ADV_SET_RANDOM_ADDR,
    OP_LE_SET_EVENT_MASK, OP_LE_SET_EXT_ADV_DATA, OP_LE_SET_EXT_ADV_ENABLE,
    OP_LE_SET_EXT_ADV_PARAMS, OP_LE_SET_EXT_SCAN_RSP_DATA,
    OP_LE_SET_RANDOM_ADDRESS, addr_str,
)
from hci_cis_cleanup import wait_disconnected
from hci_cis_pair_test import (
    IsoHci, OP_LE_SET_HOST_FEATURE, find_hci_ports, start_advertising,
    start_connection, wait_acl_pair,
)
from hci_connection_pair_logic import run_role

LE_EVENT_MASK = bytes.fromhex("ffffffffffffff1f")
SUBRATING_HOST_SUPPORT_BIT = 38
OP_LE_SET_PAST_PARAMS = 0x205C


def prepare(hci):
    hci.setup()
    hci.command(OP_LE_SET_EVENT_MASK, LE_EVENT_MASK)
    status, _ = hci.command(
        OP_LE_SET_HOST_FEATURE,
        bytes([SUBRATING_HOST_SUPPORT_BIT, 1]),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Set Host Feature bit 38 returned 0x%02X" % status)

    identity, addr_type, source = hci.identity()
    if addr_type == 1:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    return identity, addr_type, source


def connect(central, peripheral, central_type, peripheral_id, peripheral_type):
    start_advertising(peripheral, peripheral_id, peripheral_type)
    start_connection(central, central_type, peripheral_id, peripheral_type)
    return wait_acl_pair(central, peripheral)


def start_extended_advertising(hci, identity, addr_type):
    handle = 0
    adv = bytes([2, 0x01, 0x06])

    params = bytes([handle])
    params += struct.pack("<H", 0x0013)
    params += (0x0000A0).to_bytes(3, "little")
    params += (0x0000A0).to_bytes(3, "little")
    params += bytes([0x07, addr_type, 0x00])
    params += bytes(6)
    params += bytes([0x00, 0x7F, 0x01, 0x00, 0x01, 0x00, 0x00])

    status, _ = hci.command(OP_LE_SET_EXT_ADV_PARAMS, params, allow_fail=True)
    if status != 0:
        raise HciError("LE Set Extended Advertising Parameters returned 0x%02X"
                       % status)
    if addr_type == 1:
        hci.command(OP_LE_SET_ADV_SET_RANDOM_ADDR, bytes([handle]) + identity)
    hci.command(OP_LE_SET_EXT_ADV_DATA,
                bytes([handle, 0x03, 0x01, len(adv)]) + adv)
    hci.command(OP_LE_SET_EXT_SCAN_RSP_DATA,
                bytes([handle, 0x03, 0x01, 0]))
    hci.command(OP_LE_SET_EXT_ADV_ENABLE,
                bytes([0x01, 0x01, handle]) + struct.pack("<H", 0) + b"\x00")
    print("Peripheral extended advertising as %s" % addr_str(identity))


def start_extended_connection(central, central_type, peer_addr, peer_type):
    params = bytes([0x00, central_type, peer_type]) + peer_addr + b"\x01"
    params += struct.pack("<HHHHHHHH", 0x0060, 0x0030,
                          0x0018, 0x0028, 0, 400, 0, 0)
    status, _ = central.command(
        OP_LE_EXT_CREATE_CONNECTION, params, allow_fail=True)
    if status != 0:
        raise HciError("LE Extended Create Connection returned 0x%02X" % status)


def connect_extended(central, peripheral, central_type,
                     peripheral_id, peripheral_type):
    start_extended_advertising(peripheral, peripheral_id, peripheral_type)
    start_extended_connection(
        central, central_type, peripheral_id, peripheral_type)
    return wait_acl_pair(central, peripheral)


def disconnect_row(label, target, peer, target_handle, peer_handle, counts):
    status, _ = target.command(
        OP_DISCONNECT, struct.pack("<HB", target_handle, 0x13),
        allow_fail=True)
    if status != 0:
        raise HciError("%s Disconnect returned 0x%02X" % (label, status))
    if not wait_disconnected(target, target_handle):
        raise HciError("%s local Disconnection Complete missing" % label)
    if not wait_disconnected(peer, peer_handle):
        raise HciError("%s peer Disconnection Complete missing" % label)
    counts["accepted"] += 1
    print("[ok] %-10s 0x0406 Disconnect                                         complete"
          % label)


def run_past_params(label, hci, handle, counts):
    row = hci_commands.BY_OPCODE[OP_LE_SET_PAST_PARAMS]
    payload = row.build(SimpleNamespace(handle=handle))
    status, data = hci.command(row.opcode, payload, allow_fail=True)
    if status == 0:
        counts["accepted"] += 1
        result = "%d byte return" % len(data) if data else "accepted"
        print("[ok] %-10s 0x%04X %-50s %s"
              % (label, row.opcode, row.name, result))
        return
    counts["failed"] += 1
    print("[!!] %-10s 0x%04X %-50s 0x%02X"
          % (label, row.opcode, row.name, status))


def ports_for(args):
    ports = find_hci_ports()
    central = args.central
    peripheral = args.peripheral
    if central is None or peripheral is None:
        if len(ports) != 2:
            raise HciError("need two HCI ports; detected %s"
                           % (", ".join(ports) if ports else "none"))
        central = central or ports[0]
        peripheral = peripheral or ports[1]
    if central == peripheral:
        raise HciError("central and peripheral ports must differ")
    return central, peripheral


def main():
    parser = argparse.ArgumentParser(
        description="Sweep connection-scoped HCI rows on two controllers")
    parser.add_argument("--central")
    parser.add_argument("--peripheral")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    try:
        central_port, peripheral_port = ports_for(args)
    except HciError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    selected = set(
        row.opcode for row in hci_commands.COMMANDS
        if hci_commands.NEEDS_CONN in row.needs)
    selected.discard(OP_DISCONNECT)
    selected.discard(OP_LE_SET_PAST_PARAMS)
    central_selected = selected - {0x201A, 0x201B, 0xFD09}

    print("Connection-scoped HCI pair sweep")
    print("   central    %s" % central_port)
    print("   peripheral %s" % peripheral_port)

    central = IsoHci(central_port, raw=args.raw)
    peripheral = IsoHci(peripheral_port, raw=args.raw)
    handles = [None, None]
    counts = dict(accepted=0, expected=0, failed=0,
                  skipped=0, dedicated=0)

    try:
        peripheral_id, peripheral_type, peripheral_source = prepare(peripheral)
        central_id, central_type, central_source = prepare(central)
        print("Peripheral identity %s (%s)"
              % (addr_str(peripheral_id), peripheral_source))
        print("Central identity    %s (%s)"
              % (addr_str(central_id), central_source))

        print("\nCentral-role rows on a fresh legacy-family connection")
        handles[:] = connect(
            central, peripheral, central_type, peripheral_id, peripheral_type)
        run_role("central", 0, central, handles[0],
                 central_type, central_selected, counts)
        disconnect_row(
            "central", central, peripheral, handles[0], handles[1], counts)
        handles[:] = [None, None]

        print("\nPeripheral-role rows on a fresh legacy-family connection")
        handles[:] = connect(
            central, peripheral, central_type, peripheral_id, peripheral_type)
        run_role("peripheral", 1, peripheral, handles[1],
                 peripheral_type, selected, counts)
        disconnect_row(
            "peripheral", peripheral, central,
            handles[1], handles[0], counts)
        handles[:] = [None, None]

        print("\n0x205C on a fresh extended-family connection")
        peripheral_id, peripheral_type, _ = prepare(peripheral)
        central_id, central_type, _ = prepare(central)
        handles[:] = connect_extended(
            central, peripheral, central_type, peripheral_id, peripheral_type)
        run_past_params("central", central, handles[0], counts)
        run_past_params("peripheral", peripheral, handles[1], counts)
        disconnect_row(
            "extended", central, peripheral, handles[0], handles[1], counts)
        handles[:] = [None, None]

        print("\n%d accepted, %d expected refusals, %d failed, %d skipped, "
              "%d covered elsewhere."
              % (counts["accepted"], counts["expected"], counts["failed"],
                 counts["skipped"], counts["dedicated"]))
        if counts["failed"]:
            return 1
        print("PASS: controlled central/peripheral connection sweep.")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        if handles[0] is not None:
            try:
                central.command(
                    OP_DISCONNECT, struct.pack("<HB", handles[0], 0x13),
                    allow_fail=True)
            except (HciError, HciGone):
                pass
        try:
            peripheral.command(
                OP_LE_SET_ADV_ENABLE, b"\x00", allow_fail=True)
        except (HciError, HciGone):
            pass
        try:
            peripheral.command(
                OP_LE_SET_EXT_ADV_ENABLE, b"\x00\x00", allow_fail=True)
        except (HciError, HciGone):
            pass
        central.close()
        peripheral.close()


if __name__ == "__main__":
    sys.exit(main())

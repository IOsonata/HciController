#!/usr/bin/env python3
"""Reusable two-controller BLE coordination helpers."""

from pathlib import Path
import struct
import sys
import time

_TESTS_DIR = Path(__file__).resolve().parents[2]
_HARDWARE_DIR = _TESTS_DIR / "hardware"
if str(_HARDWARE_DIR) not in sys.path:
    sys.path.insert(0, str(_HARDWARE_DIR))

import serial
from serial.tools import list_ports

from hci_ble_test import (
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    H4_EVENT,
    Hci,
    HciError,
    HciGone,
    OP_DISCONNECT,
    OP_RESET,
    OP_LE_CREATE_CONNECTION,
    OP_LE_SET_ADV_DATA,
    OP_LE_SET_ADV_ENABLE,
    OP_LE_SET_ADV_PARAMS,
    OP_LE_SET_EVENT_MASK,
    OP_LE_SET_RANDOM_ADDRESS,
    OP_LE_SET_SCAN_RSP_DATA,
    addr_str,
    parse_connection,
)

DEFAULT_LE_EVENT_MASK = bytes.fromhex("ffffffffffffff1f")
DISCONNECT_REASON = 0x13

HCI_USB_VID = 0xCAFE
HCI_USB_PID = 0x4070
HCI_USB_INTERFACE = "Bluetooth HCI H:4"
HCI_USB_LOG_INTERFACE = "HCI controller log"


def _candidate_port_infos():
    candidates = []
    for info in list_ports.comports():
        if info.vid == HCI_USB_VID and info.pid == HCI_USB_PID:
            if "cu." in info.device or not sys.platform.startswith("darwin"):
                candidates.append(info)
                continue
        if info.product and "HCI Controller" in info.product:
            candidates.append(info)
    return candidates


def _interface_name(info):
    return (getattr(info, "interface", None) or "").strip()


def _metadata_hci_ports(infos):
    """Return H:4 interfaces when the OS exposes the CDC interface strings."""
    named = []
    saw_interface_metadata = False

    for info in infos:
        name = _interface_name(info)
        if not name:
            continue
        saw_interface_metadata = True
        lowered = name.lower()
        if HCI_USB_LOG_INTERFACE.lower() in lowered:
            continue
        if HCI_USB_INTERFACE.lower() in lowered or "hci h:4" in lowered:
            named.append(info.device)

    if saw_interface_metadata:
        return sorted(set(named))
    return None


def _probe_hci_port(device, timeout=0.75):
    """Identify the H:4 CDC function by asking it to execute HCI Reset."""
    hci = None
    try:
        hci = Hci(device, raw=False)
        status, _ = hci.command(OP_RESET, timeout=timeout, allow_fail=True)
        return status == 0
    except (HciError, HciGone, serial.SerialException, OSError):
        return False
    finally:
        if hci is not None:
            try:
                hci.close()
            except (serial.SerialException, OSError):
                pass


def find_hci_ports():
    """
    Return one serial device per physical HciController: its H:4 CDC port.

    HciController exposes two CDC functions under one USB device. Interface 0
    is "Bluetooth HCI H:4" and interface 1 is "HCI controller log". On hosts
    where pyserial reports the USB interface string, use it directly. Some
    macOS/pyserial combinations expose only the common product/VID/PID, so in
    that case actively probe each candidate with HCI Reset and keep only ports
    that answer as an HCI controller.
    """
    infos = _candidate_port_infos()
    by_metadata = _metadata_hci_ports(infos)
    if by_metadata is not None:
        return by_metadata

    ports = []
    for info in infos:
        if _probe_hci_port(info.device):
            ports.append(info.device)
    return sorted(set(ports))


def resolve_pair_ports(first=None, second=None):
    ports = find_hci_ports()

    if first is None and second is None:
        if len(ports) != 2:
            raise HciError(
                "need two HciController H:4 ports; detected %s"
                % (", ".join(ports) if ports else "none")
            )
        first, second = ports
    elif first is None:
        remaining = [port for port in ports if port != second]
        if len(remaining) != 1:
            raise HciError(
                "cannot choose the first H:4 port; candidates are %s"
                % (", ".join(remaining) if remaining else "none")
            )
        first = remaining[0]
    elif second is None:
        remaining = [port for port in ports if port != first]
        if len(remaining) != 1:
            raise HciError(
                "cannot choose the second H:4 port; candidates are %s"
                % (", ".join(remaining) if remaining else "none")
            )
        second = remaining[0]

    if first == second:
        raise HciError("the two HCI ports must differ")
    return first, second


def prepare_controller(hci, event_mask=DEFAULT_LE_EVENT_MASK):
    hci.setup()
    hci.command(OP_LE_SET_EVENT_MASK, event_mask)
    identity, addr_type, source = hci.identity()
    if addr_type == 0x01:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    return identity, addr_type, source


def start_legacy_advertising(hci, identity, addr_type, connectable=True):
    adv_type = 0x00 if connectable else 0x03
    params = struct.pack("<HH", 0x00A0, 0x00A0)
    params += bytes([adv_type, addr_type, 0x00])
    params += bytes(6)
    params += bytes([0x07, 0x00])
    hci.command(OP_LE_SET_ADV_PARAMS, params)

    adv = bytes([2, 0x01, 0x06])
    hci.command(
        OP_LE_SET_ADV_DATA,
        bytes([len(adv)]) + adv + bytes(31 - len(adv)),
    )
    hci.command(OP_LE_SET_SCAN_RSP_DATA, bytes(32))
    hci.command(OP_LE_SET_ADV_ENABLE, b"\x01")


def start_legacy_connection(central, central_type, peer_addr, peer_type):
    params = struct.pack("<HH", 0x0060, 0x0030)
    params += bytes([0x00, peer_type])
    params += peer_addr
    params += bytes([central_type])
    params += struct.pack("<HH", 0x0018, 0x0028)
    params += struct.pack("<HH", 0, 400)
    params += struct.pack("<HH", 0, 0)

    status, _ = central.command(
        OP_LE_CREATE_CONNECTION, params, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Create Connection returned 0x%02X" % status)


def wait_acl_pair(central, peripheral, timeout=10.0):
    handles = {"central": None, "peripheral": None}
    deadline = time.time() + timeout

    while time.time() < deadline and None in handles.values():
        for label, hci in (("central", central), ("peripheral", peripheral)):
            packet = hci.read_packet(0.1)
            if packet is None:
                continue
            kind, code, body = packet
            if kind != H4_EVENT or code != EVT_LE_META:
                continue

            info = parse_connection(body)
            if info is None:
                continue

            status, handle, role, peer, interval, _, _ = info
            if status != 0:
                raise HciError("%s connection failed: 0x%02X" % (label, status))

            expected_role = 0 if label == "central" else 1
            if role != expected_role:
                raise HciError(
                    "%s reported role %u, expected %u"
                    % (label, role, expected_role)
                )

            handles[label] = handle
            print(
                "%-10s ACL 0x%04X peer=%s interval=%.2f ms"
                % (label.capitalize(), handle, addr_str(peer), interval * 1.25)
            )

    if None in handles.values():
        raise HciError("timed out waiting for both ACL connection events")

    return handles["central"], handles["peripheral"]


def establish_legacy_acl_pair(central, peripheral):
    peripheral_id, peripheral_type, peripheral_source = prepare_controller(peripheral)
    central_id, central_type, central_source = prepare_controller(central)

    start_legacy_advertising(peripheral, peripheral_id, peripheral_type, True)
    start_legacy_connection(central, central_type, peripheral_id, peripheral_type)
    central_handle, peripheral_handle = wait_acl_pair(central, peripheral)

    return {
        "central_identity": central_id,
        "central_identity_source": central_source,
        "peripheral_identity": peripheral_id,
        "peripheral_identity_source": peripheral_source,
        "central_handle": central_handle,
        "peripheral_handle": peripheral_handle,
    }


def wait_disconnected(hci, handle, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(0.1)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_DISCONNECTION_COMPLETE:
            continue
        if len(body) < 3:
            continue
        got = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        if got == handle:
            return True
    return False


def disconnect_acl_pair(central, peripheral, central_handle, peripheral_handle):
    if central_handle is not None:
        try:
            central.command(
                OP_DISCONNECT,
                struct.pack("<HB", central_handle, DISCONNECT_REASON),
                allow_fail=True,
            )
        except (HciError, HciGone):
            pass

    for hci, handle in ((central, central_handle), (peripheral, peripheral_handle)):
        if handle is not None:
            try:
                wait_disconnected(hci, handle)
            except (HciError, HciGone):
                pass

    try:
        peripheral.command(OP_LE_SET_ADV_ENABLE, b"\x00", allow_fail=True)
    except (HciError, HciGone):
        pass

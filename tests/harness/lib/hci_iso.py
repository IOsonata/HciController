#!/usr/bin/env python3
"""Reusable H:4 ISO helpers for BLE harnesses."""

from pathlib import Path
import struct
import sys
import time

_TESTS_DIR = Path(__file__).resolve().parents[2]
_HARDWARE_DIR = _TESTS_DIR / "hardware"
if str(_HARDWARE_DIR) not in sys.path:
    sys.path.insert(0, str(_HARDWARE_DIR))

import serial

from hci_ble_test import (
    ERROR_NAMES,
    EVT_DISCONNECTION_COMPLETE,
    H4_ACL,
    H4_EVENT,
    Hci,
    HciError,
    HciGone,
)

H4_ISO = 0x05
OP_LE_READ_BUFFER_SIZE_V2 = 0x2060
OP_LE_SETUP_ISO_DATA_PATH = 0x206E

ISO_PB_FIRST = 0x00
ISO_PB_COMPLETE = 0x02
ISO_DIRECTION_INPUT = 0x00
ISO_DIRECTION_OUTPUT = 0x01
ISO_DATA_PATH_HCI = 0x00
CODING_FORMAT_TRANSPARENT = 0x03


def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))


class IsoHci(Hci):
    """HCI serial helper extended with H:4 ISO packets."""

    def read_wire(self, timeout=1.0):
        deadline = time.time() + timeout
        try:
            first = self.ser.read(1)
        except (serial.SerialException, OSError) as err:
            raise HciGone(str(err))

        if not first:
            return None

        kind = first[0]
        if kind == H4_EVENT:
            header = self.read_exact(2, deadline)
            body = self.read_exact(header[1], deadline)
            if self.raw:
                print("   rx evt %02x" % header[0], body.hex(" "))
            self.on_event(header[0], body)
            return kind, header[0], body

        if kind == H4_ACL:
            header = self.read_exact(4, deadline)
            length = struct.unpack("<H", header[2:4])[0]
            body = self.read_exact(length, deadline)
            if self.raw:
                print("   rx acl", header.hex(" "), body.hex(" "))
            return kind, None, header + body

        if kind == H4_ISO:
            header = self.read_exact(4, deadline)
            length = struct.unpack("<H", header[2:4])[0] & 0x3FFF
            body = self.read_exact(length, deadline)
            if self.raw:
                print("   rx iso", header.hex(" "), body.hex(" "))
            return kind, None, header + body

        raise HciError(
            "bad H:4 packet indicator 0x%02X, stream out of sync" % kind
        )

    def send_iso(self, handle, sequence, sdu):
        if len(sdu) > 0x0FFF:
            raise HciError("ISO SDU exceeds the HCI ISO_SDU_Length field")

        handle_flags = (handle & 0x0FFF) | (ISO_PB_COMPLETE << 12)
        load = struct.pack("<HH", sequence & 0xFFFF, len(sdu) & 0x0FFF) + sdu
        packet = struct.pack("<HH", handle_flags, len(load)) + load
        self.write_packet(bytes([H4_ISO]) + packet)


def setup_iso_path(hci, iso_handle, direction):
    payload = struct.pack("<H", iso_handle)
    payload += bytes([direction, ISO_DATA_PATH_HCI])
    payload += bytes([CODING_FORMAT_TRANSPARENT])
    payload += bytes(4)
    payload += bytes(3)
    payload += bytes([0])

    status, _ = hci.command(
        OP_LE_SETUP_ISO_DATA_PATH, payload, allow_fail=True
    )
    if status != 0:
        name = "input" if direction == ISO_DIRECTION_INPUT else "output"
        raise HciError(
            "LE Setup ISO Data Path %s returned %s"
            % (name, status_text(status))
        )


def read_iso_buffers(hci, label):
    status, data = hci.command(OP_LE_READ_BUFFER_SIZE_V2, allow_fail=True)
    if status != 0:
        raise HciError(
            "%s LE Read Buffer Size v2 returned %s"
            % (label, status_text(status))
        )
    if len(data) < 6:
        raise HciError(
            "%s LE Read Buffer Size v2 returned %d bytes"
            % (label, len(data))
        )

    acl_len, acl_count, iso_len, iso_count = struct.unpack("<HBHB", data[:6])
    print(
        "%-10s buffers ACL %d x %d, ISO %d x %d"
        % (label.capitalize(), acl_len, acl_count, iso_len, iso_count)
    )
    if iso_len == 0 or iso_count == 0:
        raise HciError("%s controller advertises no HCI ISO buffers" % label)
    return iso_len


def parse_iso(packet):
    if len(packet) < 8:
        raise HciError("short HCI ISO packet (%d bytes)" % len(packet))

    handle_flags, total = struct.unpack("<HH", packet[:4])
    total &= 0x3FFF
    if total != len(packet) - 4:
        raise HciError(
            "HCI ISO length says %d, received %d" % (total, len(packet) - 4)
        )

    handle = handle_flags & 0x0FFF
    pb = (handle_flags >> 12) & 0x03
    ts = (handle_flags >> 14) & 0x01
    at = 4
    timestamp = None
    sequence = None
    sdu_len = None
    status = None

    if pb in (ISO_PB_FIRST, ISO_PB_COMPLETE):
        if ts:
            if len(packet) < at + 4:
                raise HciError("ISO packet has TS flag without timestamp")
            timestamp = struct.unpack("<I", packet[at:at + 4])[0]
            at += 4

        if len(packet) < at + 4:
            raise HciError("ISO packet has no sequence/SDU length fields")

        sequence = struct.unpack("<H", packet[at:at + 2])[0]
        field = struct.unpack("<H", packet[at + 2:at + 4])[0]
        at += 4
        sdu_len = field & 0x0FFF
        status = (field >> 14) & 0x03

    return {
        "handle": handle,
        "pb": pb,
        "timestamp": timestamp,
        "sequence": sequence,
        "sdu_len": sdu_len,
        "status": status,
        "fragment": packet[at:],
    }


def wait_iso(hci, label, expected_handle, expected_sdu, timeout=5.0):
    deadline = time.time() + timeout
    lost = 0

    while time.time() < deadline:
        packet = hci.read_packet(0.1)
        if packet is None:
            continue
        kind, code, body = packet

        if kind == H4_ISO:
            iso = parse_iso(body)
            if iso["handle"] != expected_handle:
                continue
            if iso["status"] not in (None, 0):
                lost += 1
                continue
            if iso["pb"] != ISO_PB_COMPLETE:
                raise HciError("%s received fragmented small ISO SDU" % label)
            if iso["sdu_len"] != len(expected_sdu):
                continue
            if len(iso["fragment"]) != iso["sdu_len"]:
                raise HciError(
                    "%s HCI ISO fragment has %d bytes for declared SDU length %d"
                    % (label, len(iso["fragment"]), iso["sdu_len"])
                )
            if iso["fragment"] != expected_sdu:
                continue

            print(
                "%-10s ISO rx handle 0x%04X seq=%u len=%u status=0"
                % (
                    label.capitalize(),
                    iso["handle"],
                    iso["sequence"],
                    iso["sdu_len"],
                )
            )
            if lost:
                print(
                    "   %s reported %d lost SDU(s) before the marker"
                    % (label.capitalize(), lost)
                )
            return

        if kind == H4_EVENT and code == EVT_DISCONNECTION_COMPLETE:
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            reason = body[3] if len(body) >= 4 else 0
            raise HciError(
                "%s disconnected 0x%04X while waiting for ISO, reason %s"
                % (label, handle, status_text(reason))
            )

    raise HciError("timed out waiting for ISO marker at %s" % label)

#!/usr/bin/env python3
"""Two-controller CIS + HCI ISO data-path hardware test."""

import argparse
import struct
import sys
import time

from serial.tools import list_ports

import hci_commands
# The teardown lives in hci_cis_cleanup so it can be driven without a board.
from hci_cis_cleanup import cleanup

from hci_ble_test import (
    ERROR_NAMES,
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    H4_ACL,
    H4_EVENT,
    Hci,
    HciError,
    HciGone,
    OP_DISCONNECT,
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

H4_ISO = 0x05

LE_CIS_ESTABLISHED_SUBEVENTS = (0x19, 0x2A)
LE_CIS_REQUEST = 0x1A

OP_LE_READ_BUFFER_SIZE_V2 = 0x2060
OP_LE_SET_CIG_PARAMS = 0x2062
OP_LE_CREATE_CIS = 0x2064
OP_LE_REMOVE_CIG = 0x2065
OP_LE_ACCEPT_CIS_REQUEST = 0x2066
OP_LE_SETUP_ISO_DATA_PATH = 0x206E
OP_LE_REMOVE_ISO_DATA_PATH = 0x206F
OP_LE_SET_HOST_FEATURE = 0x2074

CIS_HOST_SUPPORT_BIT = 32
CODING_FORMAT_TRANSPARENT = 0x03

ISO_PB_FIRST = 0x00
ISO_PB_COMPLETE = 0x02
ISO_DIRECTION_INPUT = 0x00
ISO_DIRECTION_OUTPUT = 0x01
ISO_DATA_PATH_HCI = 0x00
ISO_PACKET_STATUS_NAMES = {
    1: "possibly invalid",
    2: "lost",
    3: "reserved-status",
}

# CIS Established v1/v2 are LE subevents 0x19 and 0x2A; CIS Request is 0x1A.
# Their event mask bits are therefore 24, 41 and 25. The existing probe mask
# already covers all three, along with the rest of the LE events this test may see.
LE_EVENT_MASK_WITH_CIS = bytes.fromhex("ffffffffffffff1f")


class IsoHci(Hci):
    """The hardware HCI helper with H:4 ISO transmit support."""

    def send_iso(self, handle, sequence, sdu):
        if len(sdu) > 0x0FFF:
            raise HciError("ISO SDU exceeds the HCI ISO_SDU_Length field")

        # PB=0b10 means one complete SDU. TS=0 means the load begins with
        # Packet_Sequence_Number and ISO_SDU_Length/Packet_Status_Flag.
        handle_flags = (handle & 0x0FFF) | (ISO_PB_COMPLETE << 12)
        load = struct.pack("<HH", sequence & 0xFFFF,
                           len(sdu) & 0x0FFF) + sdu
        packet = struct.pack("<HH", handle_flags, len(load)) + load
        self.write_packet(bytes([H4_ISO]) + packet)


def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))


def find_hci_ports():
    ports = []
    for info in list_ports.comports():
        if info.vid == 0xCAFE and info.pid == 0x4070:
            if "cu." in info.device or not sys.platform.startswith("darwin"):
                ports.append(info.device)
                continue
        if info.product and "HCI" in info.product:
            ports.append(info.device)
    return sorted(set(ports))


def prepare(hci):
    hci.setup()
    hci.command(OP_LE_SET_EVENT_MASK, LE_EVENT_MASK_WITH_CIS)

    status, _ = hci.command(
        OP_LE_SET_HOST_FEATURE,
        bytes([CIS_HOST_SUPPORT_BIT, 1]),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Set Host Feature bit 32 returned %s"
                       % status_text(status))

    identity, addr_type, source = hci.identity()
    if addr_type == 0x01:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    return identity, addr_type, source


def start_advertising(hci, identity, addr_type):
    params = struct.pack("<HH", 0x00A0, 0x00A0)
    params += bytes([0x00, addr_type, 0x00])
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
    print("Peripheral advertising as %s" % addr_str(identity))


def start_connection(central, central_type, peer_addr, peer_type):
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
        raise HciError("LE Create Connection returned %s" % status_text(status))


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
                raise HciError(
                    "%s ACL connection failed: %s"
                    % (label, status_text(status))
                )

            expected_role = 0 if label == "central" else 1
            if role != expected_role:
                raise HciError(
                    "%s ACL connection reported role %u, expected %u"
                    % (label, role, expected_role)
                )

            handles[label] = handle
            print(
                "%-10s ACL 0x%04X role=%s peer=%s interval=%.2f ms"
                % (
                    label.capitalize(),
                    handle,
                    "central" if role == 0 else "peripheral",
                    addr_str(peer),
                    interval * 1.25,
                )
            )

    if None in handles.values():
        raise HciError("timed out waiting for both ACL connection events")

    return handles["central"], handles["peripheral"]


def parse_cig_return(data):
    if len(data) < 2:
        raise HciError("LE Set CIG Parameters returned only %d bytes" % len(data))

    cig_id = data[0]
    count = data[1]
    expected = 2 + (count * 2)
    if len(data) != expected:
        raise HciError(
            "CIG return says %d CIS but returned %d bytes"
            % (count, len(data))
        )

    handles = [
        struct.unpack("<H", data[2 + 2 * i:4 + 2 * i])[0] & 0x0FFF
        for i in range(count)
    ]
    return cig_id, handles


def create_cig(central):
    row = hci_commands.BY_OPCODE[OP_LE_SET_CIG_PARAMS]
    payload = row.build(None)
    if len(payload) < 16 or payload[14] != 1:
        raise HciError("CIG test row does not describe exactly one CIS")
    expected_cis_id = payload[15]

    status, data = central.command(
        OP_LE_SET_CIG_PARAMS, payload, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Set CIG Parameters returned %s"
                       % status_text(status))

    cig_id, handles = parse_cig_return(data)
    if len(handles) != 1:
        raise HciError("expected one CIS handle, got %d" % len(handles))

    print("CIG 0x%02X reserved central CIS 0x%04X"
          % (cig_id, handles[0]))
    return cig_id, handles[0], expected_cis_id


def wait_cis_request(peripheral, expected_acl, expected_cig, expected_cis_id,
                     timeout=5.0):
    deadline = time.time() + timeout

    while time.time() < deadline:
        packet = peripheral.read_packet(0.1)
        if packet is None:
            continue

        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_DISCONNECTION_COMPLETE:
            raise HciError("ACL disconnected while waiting for CIS Request")

        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        if len(body) < 7 or body[0] != LE_CIS_REQUEST:
            continue

        acl = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        cis = struct.unpack("<H", body[3:5])[0] & 0x0FFF
        if acl != expected_acl:
            continue

        cig = body[5]
        cis_id = body[6]
        if cig != expected_cig or cis_id != expected_cis_id:
            raise HciError(
                "CIS Request reported CIG %u CIS_ID %u, expected %u/%u"
                % (cig, cis_id, expected_cig, expected_cis_id)
            )

        print(
            "Peripheral CIS Request: ACL 0x%04X CIS 0x%04X CIG %u CIS_ID %u"
            % (acl, cis, cig, cis_id)
        )
        return cis

    raise HciError("timed out waiting for LE CIS Request")


def wait_cis_established(hci, label, expected_cis, timeout=5.0):
    deadline = time.time() + timeout

    while time.time() < deadline:
        packet = hci.read_packet(0.1)
        if packet is None:
            continue

        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_DISCONNECTION_COMPLETE:
            raise HciError("%s disconnected while establishing CIS" % label)

        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        if len(body) < 4 or body[0] not in LE_CIS_ESTABLISHED_SUBEVENTS:
            continue

        status = body[1]
        cis = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        if cis != expected_cis:
            continue
        if status != 0:
            raise HciError(
                "%s LE CIS Established returned %s"
                % (label, status_text(status))
            )

        print("%-10s CIS established on 0x%04X"
              % (label.capitalize(), cis))
        return

    raise HciError("timed out waiting for %s LE CIS Established" % label)


def setup_iso_path(hci, cis, direction):
    payload = struct.pack("<H", cis)
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
        raise HciError("%s LE Read Buffer Size v2 returned %s"
                       % (label, status_text(status)))
    if len(data) < 6:
        raise HciError("%s LE Read Buffer Size v2 returned %d bytes"
                       % (label, len(data)))

    acl_len, acl_count, iso_len, iso_count = struct.unpack("<HBHB", data[:6])
    print("%-10s buffers ACL %d x %d, ISO %d x %d"
          % (label.capitalize(), acl_len, acl_count, iso_len, iso_count))

    if iso_len == 0 or iso_count == 0:
        raise HciError("%s controller advertises no HCI ISO buffers" % label)

    return iso_len


def parse_iso(packet):
    if len(packet) < 8:
        raise HciError("short HCI ISO packet (%d bytes)" % len(packet))

    handle_flags, total = struct.unpack("<HH", packet[:4])
    total &= 0x3FFF
    if total != len(packet) - 4:
        raise HciError("HCI ISO length says %d, received %d"
                       % (total, len(packet) - 4))

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


def wait_iso(hci, label, expected_cis, expected_sdu, timeout=5.0):
    deadline = time.time() + timeout
    nonvalid = {}

    while time.time() < deadline:
        packet = hci.read_packet(0.1)
        if packet is None:
            continue

        kind, code, body = packet

        if kind == H4_ISO:
            iso = parse_iso(body)
            if iso["handle"] != expected_cis:
                continue

            if iso["status"] not in (None, 0):
                packet_status = iso["status"]
                nonvalid[packet_status] = nonvalid.get(packet_status, 0) + 1
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
            if nonvalid:
                details = []
                for packet_status in sorted(nonvalid):
                    name = ISO_PACKET_STATUS_NAMES.get(
                        packet_status, "status-%u" % packet_status)
                    details.append("%d %s" % (nonvalid[packet_status], name))
                print("   %s reported %s HCI ISO packet(s) before the marker"
                      % (label.capitalize(), ", ".join(details)))
            return

        if kind == H4_EVENT and code == EVT_DISCONNECTION_COMPLETE:
            handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            reason = body[3] if len(body) >= 4 else 0
            raise HciError(
                "%s disconnected 0x%04X while waiting for ISO, reason %s"
                % (label, handle, status_text(reason))
            )

    raise HciError("timed out waiting for ISO marker at %s" % label)


def counter_snapshot(hci):
    return hci.read_counters()


def counter_delta(before, after, index):
    if before is None or after is None:
        return None
    if index >= len(before) or index >= len(after):
        return None
    return after[index] - before[index]


def main():
    parser = argparse.ArgumentParser(
        description="Create a CIS between two controllers and move HCI ISO SDUs"
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
            print(
                "Need two HCI serial ports. Detected: %s"
                % (", ".join(ports) if ports else "none"),
                file=sys.stderr,
            )
            print("Use --central PORT --peripheral PORT.", file=sys.stderr)
            return 2

    if central_port == peripheral_port:
        print("Central and peripheral ports must differ.", file=sys.stderr)
        return 2

    print("CIS/ISO pair test")
    print("   central    %s" % central_port)
    print("   peripheral %s" % peripheral_port)

    central = IsoHci(central_port, raw=args.raw)
    peripheral = IsoHci(peripheral_port, raw=args.raw)

    cig_id = None
    central_cis = None
    peripheral_cis = None
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
        start_connection(
            central, central_type, peripheral_id, peripheral_type
        )
        central_acl, peripheral_acl = wait_acl_pair(central, peripheral)

        cig_id, central_cis, cis_id = create_cig(central)

        status, _ = central.command(
            OP_LE_CREATE_CIS,
            bytes([1]) + struct.pack("<HH", central_cis, central_acl),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Create CIS returned %s" % status_text(status))
        print("Central LE Create CIS accepted")

        peripheral_cis = wait_cis_request(
            peripheral, peripheral_acl, cig_id, cis_id
        )
        status, _ = peripheral.command(
            OP_LE_ACCEPT_CIS_REQUEST,
            struct.pack("<H", peripheral_cis),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Accept CIS Request returned %s"
                           % status_text(status))
        print("Peripheral LE Accept CIS Request accepted")

        wait_cis_established(
            peripheral, "peripheral", peripheral_cis
        )
        wait_cis_established(
            central, "central", central_cis
        )

        for hci, cis, label in (
            (central, central_cis, "central"),
            (peripheral, peripheral_cis, "peripheral"),
        ):
            setup_iso_path(hci, cis, ISO_DIRECTION_INPUT)
            setup_iso_path(hci, cis, ISO_DIRECTION_OUTPUT)
            print("%-10s HCI ISO input/output paths ready"
                  % label.capitalize())

        central_iso = read_iso_buffers(central, "central")
        peripheral_iso = read_iso_buffers(peripheral, "peripheral")

        c2p = b"CIS central->peripheral"
        p2c = b"CIS peripheral->central"
        needed = max(len(c2p), len(p2c)) + 4
        if central_iso < needed or peripheral_iso < needed:
            raise HciError("HCI ISO buffer is too small for the test SDUs")

        central_before = counter_snapshot(central)
        peripheral_before = counter_snapshot(peripheral)

        central.send_iso(central_cis, 0, c2p)
        wait_iso(peripheral, "peripheral", peripheral_cis, c2p)
        print("[ok] central -> peripheral ISO SDU matches")

        peripheral.send_iso(peripheral_cis, 0, p2c)
        wait_iso(central, "central", central_cis, p2c)
        print("[ok] peripheral -> central ISO SDU matches")

        central_after = counter_snapshot(central)
        peripheral_after = counter_snapshot(peripheral)

        central_taken = counter_delta(central_before, central_after, 17)
        peripheral_taken = counter_delta(
            peripheral_before, peripheral_after, 17
        )
        central_drop = counter_delta(central_before, central_after, 14)
        peripheral_drop = counter_delta(
            peripheral_before, peripheral_after, 14
        )

        if central_taken is not None:
            print("Central ISO taken +%d, dropped +%d"
                  % (central_taken, central_drop))
        if peripheral_taken is not None:
            print("Peripheral ISO taken +%d, dropped +%d"
                  % (peripheral_taken, peripheral_drop))

        if central_drop not in (None, 0) or peripheral_drop not in (None, 0):
            raise HciError("the routing layer dropped an ISO packet")
        if central_taken is not None and central_taken < 1:
            raise HciError("central ISO packet never reached SDC")
        if peripheral_taken is not None and peripheral_taken < 1:
            raise HciError("peripheral ISO packet never reached SDC")

        print()
        print("PASS: ACL, CIS establishment, HCI ISO data paths, and")
        print("bidirectional ISO SDU transfer all work over the air.")
        return 0

    except (HciError, HciGone) as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 1
    finally:
        cleanup(
            central,
            peripheral,
            cig_id,
            central_cis,
            peripheral_cis,
            central_acl,
            peripheral_acl,
        )
        central.close()
        peripheral.close()


if __name__ == "__main__":
    sys.exit(main())

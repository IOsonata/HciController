#!/usr/bin/env python3
"""Direct LE Extended Create Connection diagnostic for HciController.

This reproduces the IOsonata BleGateway central sequence from a PC host:
extended active scan on LE 1M, scan disable, then opcode 0x2043 using the
address and address type observed in the advertising report.

The test stops at link establishment. It does not run GATT discovery, so a
failure here belongs below the IOsonata GATT/application layer.
"""

import argparse
import struct
import sys
import time

import _bootstrap  # noqa: F401

from hcicontroller import (
    Hci,
    HciError,
    HciGone,
    SelectionError,
    TransportSpec,
    addr_bytes,
    addr_str,
    discover,
)
from hcicontroller.hci_events import (
    ERROR_NAMES,
    EVT_DISCONNECTION_COMPLETE,
    EVT_LE_META,
    H4_EVENT,
    LE_EXTENDED_ADVERTISING_REPORT,
    describe_interval,
    parse_connection,
)


OP_LE_SET_EVENT_MASK = 0x2001
OP_LE_SET_RANDOM_ADDRESS = 0x2005
OP_LE_SET_EXT_SCAN_PARAMS = 0x2041
OP_LE_SET_EXT_SCAN_ENABLE = 0x2042
OP_LE_EXT_CREATE_CONNECTION = 0x2043
OP_DISCONNECT = 0x0406
OP_RESET = 0x0C03

PHY_1M = 0x01
LE_EVENT_MASK = bytes.fromhex("ffffffffffffff1f")

# Match exemples/gateway/ble_gateway.cpp.
SCAN_INTERVAL = 0x0640       # 1000 ms / 0.625 ms
SCAN_WINDOW = 0x00A0         # 100 ms / 0.625 ms
CONN_INTERVAL_MIN = 0x0018   # 30 ms / 1.25 ms
CONN_INTERVAL_MAX = 0x003C   # 75 ms / 1.25 ms
CONN_LATENCY = 0
SUPERVISION_TIMEOUT = 0x0190  # 4000 ms / 10 ms
MIN_CE_LENGTH = 0
MAX_CE_LENGTH = 0

ADDR_TYPE_NAMES = {
    0x00: "public",
    0x01: "random",
    0x02: "public identity",
    0x03: "random identity",
    0xFF: "anonymous",
}


def _signed_octet(value):
    return value - 256 if value >= 128 else value


def _event_type_text(event_type):
    names = []
    if event_type & 0x0001:
        names.append("connectable")
    if event_type & 0x0002:
        names.append("scannable")
    if event_type & 0x0004:
        names.append("directed")
    if event_type & 0x0008:
        names.append("scan-response")
    if event_type & 0x0010:
        names.append("legacy")
    data_status = (event_type >> 5) & 0x03
    names.append("data-status=%u" % data_status)
    return ",".join(names)


def _extended_reports(body):
    """Decode every report in one LE Extended Advertising Report event."""
    if len(body) < 2 or body[0] != LE_EXTENDED_ADVERTISING_REPORT:
        return

    count = body[1]
    offset = 2

    for index in range(count):
        if len(body) < offset + 24:
            raise HciError(
                "truncated extended advertising report %u/%u: %s"
                % (index + 1, count, body.hex(" "))
            )

        event_type = struct.unpack("<H", body[offset:offset + 2])[0]
        addr_type = body[offset + 2]
        address = body[offset + 3:offset + 9]
        primary_phy = body[offset + 9]
        secondary_phy = body[offset + 10]
        sid = body[offset + 11]
        tx_power = _signed_octet(body[offset + 12])
        rssi = _signed_octet(body[offset + 13])
        periodic_interval = struct.unpack(
            "<H", body[offset + 14:offset + 16]
        )[0]
        direct_addr_type = body[offset + 16]
        direct_address = body[offset + 17:offset + 23]
        data_len = body[offset + 23]
        end = offset + 24 + data_len

        if len(body) < end:
            raise HciError(
                "truncated advertising data %u/%u: data_len=%u raw=%s"
                % (index + 1, count, data_len, body.hex(" "))
            )

        yield {
            "event_type": event_type,
            "addr_type": addr_type,
            "address": address,
            "primary_phy": primary_phy,
            "secondary_phy": secondary_phy,
            "sid": sid,
            "tx_power": tx_power,
            "rssi": rssi,
            "periodic_interval": periodic_interval,
            "direct_addr_type": direct_addr_type,
            "direct_address": direct_address,
            "data": body[offset + 24:end],
        }

        offset = end

    if offset != len(body):
        raise HciError(
            "extended advertising event has %u trailing byte(s): %s"
            % (len(body) - offset, body.hex(" "))
        )


def _resolve_transport(args):
    if args.port and args.usb:
        raise SelectionError("--port and --usb are mutually exclusive")

    if args.port:
        if args.transport == "usb":
            raise SelectionError("--port cannot be used with --transport usb")
        return TransportSpec(
            "serial", args.port, "serial H:4 %s" % args.port
        )

    if args.usb:
        if args.transport == "serial":
            raise SelectionError("--usb cannot be used with --transport serial")
        return discover(kind="usb", usb_selector=args.usb)

    return discover(kind=args.transport)


def _command(hci, opcode, params, name):
    print("Host -> Controller: %s (0x%04X)" % (name, opcode))
    print("   params:", params.hex(" ") if params else "<none>")
    status, data = hci.command(opcode, params, allow_fail=True)
    print(
        "Controller -> Host: %s status=0x%02X %s"
        % (name, status, ERROR_NAMES.get(status, ""))
    )
    return status, data


def _set_random_address_if_needed(hci, identity, own_addr_type):
    if own_addr_type != 0x01:
        return

    status, _ = _command(
        hci,
        OP_LE_SET_RANDOM_ADDRESS,
        identity,
        "LE Set Random Address",
    )
    if status != 0:
        raise HciError("LE Set Random Address failed")


def _scan_for_peer(hci, target, own_addr_type, seconds):
    scan_params = bytes([own_addr_type, 0x00, PHY_1M, 0x01])
    scan_params += struct.pack("<HH", SCAN_INTERVAL, SCAN_WINDOW)

    status, _ = _command(
        hci,
        OP_LE_SET_EXT_SCAN_PARAMS,
        scan_params,
        "LE Set Extended Scan Parameters",
    )
    if status != 0:
        raise HciError("LE Set Extended Scan Parameters failed")

    scan_enable = bytes([0x01, 0x00]) + struct.pack("<HH", 0, 0)
    status, _ = _command(
        hci,
        OP_LE_SET_EXT_SCAN_ENABLE,
        scan_enable,
        "LE Set Extended Scan Enable",
    )
    if status != 0:
        raise HciError("LE Set Extended Scan Enable failed")

    print(
        "Scanning for %s for up to %.1f s..."
        % (addr_str(target), seconds)
    )
    deadline = time.monotonic() + seconds
    found = None

    while time.monotonic() < deadline:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue

        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        if not body or body[0] != LE_EXTENDED_ADVERTISING_REPORT:
            continue

        for report in _extended_reports(body):
            if report["address"] != target:
                continue

            if found is None or (report["event_type"] & 0x0001):
                found = report
            print("Target advertising report:")
            print(
                "   addr=%s type=0x%02X (%s) rssi=%d dBm"
                % (
                    addr_str(report["address"]),
                    report["addr_type"],
                    ADDR_TYPE_NAMES.get(report["addr_type"], "unknown"),
                    report["rssi"],
                )
            )
            print(
                "   event_type=0x%04X [%s]"
                % (
                    report["event_type"],
                    _event_type_text(report["event_type"]),
                )
            )
            print(
                "   primary_phy=0x%02X secondary_phy=0x%02X sid=0x%02X "
                "tx_power=%d data_len=%u"
                % (
                    report["primary_phy"],
                    report["secondary_phy"],
                    report["sid"],
                    report["tx_power"],
                    len(report["data"]),
                )
            )
            if report["event_type"] & 0x0001:
                break

        if found is not None and (found["event_type"] & 0x0001):
            break

    scan_disable = bytes([0x00, 0x00]) + struct.pack("<HH", 0, 0)
    status, _ = _command(
        hci,
        OP_LE_SET_EXT_SCAN_ENABLE,
        scan_disable,
        "LE Set Extended Scan Enable (disable)",
    )
    if status != 0:
        raise HciError("LE Set Extended Scan Enable disable failed")

    # Reports can race the disable Command Complete. Hci.command() preserves
    # those asynchronous events in pending; discard only stale advertising
    # reports before initiation so the connection trace starts at 0x2043.
    keep = []
    while hci.pending:
        packet = hci.pending.pop(0)
        kind, code, body = packet
        if (
            kind == H4_EVENT
            and code == EVT_LE_META
            and body
            and body[0] == LE_EXTENDED_ADVERTISING_REPORT
        ):
            continue
        keep.append(packet)
    hci.pending.extend(keep)

    return found


def _extended_create_params(own_addr_type, report):
    params = bytes([
        0x00,                   # Initiator_Filter_Policy: peer address
        own_addr_type,
        report["addr_type"],
    ])
    params += report["address"]
    params += bytes([PHY_1M])
    params += struct.pack(
        "<HHHHHHHH",
        SCAN_INTERVAL,
        SCAN_WINDOW,
        CONN_INTERVAL_MIN,
        CONN_INTERVAL_MAX,
        CONN_LATENCY,
        SUPERVISION_TIMEOUT,
        MIN_CE_LENGTH,
        MAX_CE_LENGTH,
    )
    if len(params) != 26:
        raise AssertionError(
            "LE Extended Create Connection payload is %u bytes, expected 26"
            % len(params)
        )
    return params


def _wait_for_connection(hci, target, seconds):
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue

        kind, code, body = packet
        if kind != H4_EVENT:
            print(
                "Controller -> Host: packet type=0x%02X code=0x%02X len=%u"
                % (kind, code, len(body))
            )
            continue

        if code == EVT_LE_META:
            info = parse_connection(body)
            if info is None:
                subevent = body[0] if body else 0xFF
                print(
                    "Controller -> Host: LE Meta subevent=0x%02X len=%u"
                    % (subevent, len(body))
                )
                continue

            status, handle, role, peer, interval, latency, timeout = info
            print(
                "Controller -> Host: LE connection complete status=0x%02X %s"
                % (status, ERROR_NAMES.get(status, ""))
            )
            print(
                "   handle=0x%04X role=%u peer=%s"
                % (handle, role, addr_str(peer))
            )
            if status != 0:
                return None, status

            print("   " + describe_interval(interval, latency, timeout))
            if peer != target:
                print(
                    "   WARNING: completed peer differs from requested %s"
                    % addr_str(target)
                )
            return handle, status

        if code == EVT_DISCONNECTION_COMPLETE:
            print(
                "Controller -> Host: unexpected Disconnection Complete %s"
                % body.hex(" ")
            )
        else:
            print(
                "Controller -> Host: event=0x%02X len=%u body=%s"
                % (code, len(body), body.hex(" "))
            )

    return None, None


def _disconnect(hci, handle):
    params = struct.pack("<HB", handle, 0x13)
    status, _ = _command(hci, OP_DISCONNECT, params, "Disconnect")
    if status != 0:
        return

    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_DISCONNECTION_COMPLETE:
            continue
        if len(body) < 4:
            continue
        got_handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        if got_handle == handle:
            print(
                "Controller -> Host: Disconnection Complete status=0x%02X "
                "reason=0x%02X"
                % (body[0], body[3])
            )
            return


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Reproduce BleGateway LE Extended Create Connection directly "
            "from a PC HCI host"
        )
    )
    parser.add_argument(
        "address",
        help="target advertiser address, for example e9:6c:5a:69:9b:5b",
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "serial", "usb"),
        default="auto",
    )
    parser.add_argument("--port", help="explicit UART/CDC H:4 port")
    parser.add_argument("--usb", help="native USB HCI serial number or VID:PID")
    parser.add_argument(
        "--scan-seconds",
        type=float,
        default=15.0,
        help="time to wait for the target advertising report",
    )
    parser.add_argument(
        "--connect-seconds",
        type=float,
        default=30.0,
        help="time to wait for LE connection completion",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="print every raw HCI packet in addition to the decoded trace",
    )
    args = parser.parse_args()

    if args.scan_seconds <= 0 or args.connect_seconds <= 0:
        parser.error("scan and connection timeouts must be positive")

    try:
        target = addr_bytes(args.address)
        spec = _resolve_transport(args)
    except (ValueError, SelectionError) as err:
        print("FAIL:", err, file=sys.stderr)
        return 2

    if spec is None:
        print("FAIL: no HciController transport found", file=sys.stderr)
        return 2

    print("Transport:", spec)
    print("Target:", addr_str(target))
    print(
        "Gateway parameters: active 1M scan %.1f/%.1f ms, "
        "connection %.1f-%.1f ms, latency %u, supervision %u ms"
        % (
            SCAN_INTERVAL * 0.625,
            SCAN_WINDOW * 0.625,
            CONN_INTERVAL_MIN * 1.25,
            CONN_INTERVAL_MAX * 1.25,
            CONN_LATENCY,
            SUPERVISION_TIMEOUT * 10,
        )
    )

    hci = None
    initiating = False
    handle = None

    try:
        hci = Hci(spec, raw=args.raw)
        hci.setup()
        status, _ = _command(
            hci,
            OP_LE_SET_EVENT_MASK,
            LE_EVENT_MASK,
            "LE Set Event Mask (include Enhanced Connection Complete v2)",
        )
        if status != 0:
            raise HciError("LE Set Event Mask failed")

        identity, own_addr_type, source = hci.identity()
        print(
            "Controller identity: %s type=0x%02X (%s)"
            % (addr_str(identity), own_addr_type, source)
        )
        _set_random_address_if_needed(hci, identity, own_addr_type)

        report = _scan_for_peer(
            hci, target, own_addr_type, args.scan_seconds
        )
        if report is None:
            print("FAIL: target was not observed while extended scanning")
            return 1

        if (report["event_type"] & 0x0001) == 0:
            print(
                "WARNING: target report is not marked connectable; "
                "the gateway currently does not receive this flag either."
            )

        params = _extended_create_params(own_addr_type, report)
        print()
        print(
            "Create request uses peer type exactly as reported by scanning: "
            "0x%02X (%s)"
            % (
                report["addr_type"],
                ADDR_TYPE_NAMES.get(report["addr_type"], "unknown"),
            )
        )

        status, _ = _command(
            hci,
            OP_LE_EXT_CREATE_CONNECTION,
            params,
            "LE Extended Create Connection",
        )
        if status != 0:
            print("FAIL: controller rejected opcode 0x2043 immediately")
            return 1

        initiating = True
        print(
            "0x2043 accepted; waiting %.1f s for LE connection completion..."
            % args.connect_seconds
        )
        handle, completion_status = _wait_for_connection(
            hci, target, args.connect_seconds
        )
        initiating = completion_status is None

        if handle is None:
            if completion_status is None:
                print(
                    "FAIL: 0x2043 returned success but no LE "
                    "connection-complete event arrived"
                )
            else:
                print(
                    "FAIL: controller reported connection failure "
                    "status=0x%02X %s"
                    % (
                        completion_status,
                        ERROR_NAMES.get(completion_status, ""),
                    )
                )
            return 1

        print("PASS: HciController established the BLE link via opcode 0x2043")
        _disconnect(hci, handle)
        handle = None
        return 0

    except (HciError, HciGone) as err:
        print("FAIL:", err)
        return 1
    finally:
        if hci is not None:
            # A diagnostic timeout is not a gateway workaround. Reset only
            # after the result has been recorded so an accepted initiation
            # cannot remain active after the PC test exits.
            if initiating:
                print(
                    "Cleanup: resetting controller after timed-out initiation "
                    "(no LE Create Connection Cancel used)."
                )
                try:
                    hci.command(OP_RESET, allow_fail=True)
                except (HciError, HciGone):
                    pass
            hci.close()


if __name__ == "__main__":
    sys.exit(main())

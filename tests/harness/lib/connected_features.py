#!/usr/bin/env python3
"""Stateful two-controller feature procedures used by the release harness."""

from pathlib import Path
import struct
import sys
import time

_TESTS_DIR = Path(__file__).resolve().parents[2]
_HARDWARE_DIR = _TESTS_DIR / "hardware"
if str(_HARDWARE_DIR) not in sys.path:
    sys.path.insert(0, str(_HARDWARE_DIR))

from hci_events import (
    EVT_LE_META,
    H4_ACL,
    H4_EVENT,
    HciError,
    HciGone,
    status_text,
)

from .hci_pair import (
    disconnect_acl_pair,
    prepare_controller,
    start_legacy_advertising,
    start_legacy_connection,
    wait_acl_pair,
)

OP_READ_RSSI = 0x1405
OP_LE_READ_REMOTE_FEATURES = 0x2016
OP_LE_SET_DATA_LENGTH = 0x2022
OP_LE_READ_PHY = 0x2030
OP_LE_SET_PHY = 0x2032
OP_LE_REQUEST_PEER_SCA = 0x206D
OP_LE_SET_HOST_FEATURE = 0x2074
OP_LE_ENHANCED_READ_TX_POWER = 0x2076
OP_LE_READ_REMOTE_TX_POWER = 0x2077
OP_LE_SET_PATH_LOSS_PARAMS = 0x2078
OP_LE_SET_PATH_LOSS_ENABLE = 0x2079
OP_LE_SET_TX_POWER_REPORT_ENABLE = 0x207A
OP_LE_SUBRATE_REQUEST = 0x207E

LE_READ_REMOTE_FEATURES_COMPLETE = 0x04
LE_DATA_LENGTH_CHANGE = 0x07
LE_PHY_UPDATE_COMPLETE = 0x0C
LE_REQUEST_PEER_SCA_COMPLETE = 0x1F
LE_PATH_LOSS_THRESHOLD = 0x20
LE_TRANSMIT_POWER_REPORTING = 0x21
LE_SUBRATE_CHANGE = 0x23

PHY_1M = 0x01
PHY_2M = 0x02
TEST_CID = 0x0040
SUBRATING_HOST_SUPPORT_BIT = 38


def _restore(hci, packets):
    if packets:
        hci.pending = packets + hci.pending


def wait_le_subevent(hci, subevent, handle=None, handle_offset=None,
                     predicate=None, timeout=5.0):
    """Wait for one LE Meta subevent while preserving unrelated packets."""
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META and body and body[0] == subevent:
            matches = True
            if handle is not None:
                if handle_offset is None or len(body) < handle_offset + 2:
                    matches = False
                else:
                    got = struct.unpack("<H", body[handle_offset:handle_offset + 2])[0] & 0x0FFF
                    matches = got == handle
            if matches and (predicate is None or predicate(body)):
                _restore(hci, deferred)
                return body
        deferred.append(packet)
    _restore(hci, deferred)
    return None


def wait_acl_marker(hci, expected_handle, expected_cid, expected_payload,
                    timeout=5.0):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, _, body = packet
        if kind != H4_ACL or len(body) < 8:
            deferred.append(packet)
            continue

        handle_flags, total = struct.unpack("<HH", body[:4])
        handle = handle_flags & 0x0FFF
        if total != len(body) - 4 or handle != expected_handle:
            deferred.append(packet)
            continue

        l2cap_len, cid = struct.unpack("<HH", body[4:8])
        payload = body[8:8 + l2cap_len]
        if cid == expected_cid and payload == expected_payload:
            _restore(hci, deferred)
            return True
        deferred.append(packet)

    _restore(hci, deferred)
    return False


def _read_rssi(hci, handle):
    status, data = hci.command(OP_READ_RSSI, struct.pack("<H", handle), allow_fail=True)
    if status != 0 or len(data) < 3:
        raise HciError("Read RSSI returned %s" % status_text(status))
    got = struct.unpack("<H", data[:2])[0] & 0x0FFF
    if got != handle:
        raise HciError("Read RSSI returned handle 0x%04X, expected 0x%04X" % (got, handle))
    return struct.unpack("<b", data[2:3])[0]


def _read_remote_features(hci, handle):
    status, _ = hci.command(OP_LE_READ_REMOTE_FEATURES, struct.pack("<H", handle), allow_fail=True)
    if status != 0:
        raise HciError("LE Read Remote Features returned %s" % status_text(status))
    body = wait_le_subevent(
        hci, LE_READ_REMOTE_FEATURES_COMPLETE, handle, 2,
        predicate=lambda event: len(event) >= 12 and event[1] == 0,
    )
    if body is None:
        raise HciError("no successful LE Read Remote Features Complete event")
    return body[4:12]


def _set_data_length(hci, handle):
    payload = struct.pack("<HHH", handle, 251, 2120)
    status, _ = hci.command(OP_LE_SET_DATA_LENGTH, payload, allow_fail=True)
    if status != 0:
        raise HciError("LE Set Data Length returned %s" % status_text(status))
    body = wait_le_subevent(
        hci, LE_DATA_LENGTH_CHANGE, handle, 1,
        predicate=lambda event: len(event) >= 11,
    )
    if body is None:
        raise HciError("no LE Data Length Change event")
    tx_octets = struct.unpack("<H", body[3:5])[0]
    rx_octets = struct.unpack("<H", body[7:9])[0]
    return tx_octets, rx_octets


def _set_phy(hci, handle, phy):
    payload = struct.pack("<HBBBH", handle, 0, phy, phy, 0)
    status, _ = hci.command(OP_LE_SET_PHY, payload, allow_fail=True)
    if status != 0:
        raise HciError("LE Set PHY returned %s" % status_text(status))

    def expected(event):
        return len(event) >= 6 and event[1] == 0 and event[4] == phy and event[5] == phy

    body = wait_le_subevent(hci, LE_PHY_UPDATE_COMPLETE, handle, 2,
                            predicate=expected)
    if body is None:
        raise HciError("no successful PHY Update Complete to PHY %u" % phy)

    status, data = hci.command(OP_LE_READ_PHY, struct.pack("<H", handle), allow_fail=True)
    if status != 0 or len(data) < 4:
        raise HciError("LE Read PHY failed after PHY update")
    if data[2] != phy or data[3] != phy:
        raise HciError("LE Read PHY reports tx %u rx %u, expected %u" % (data[2], data[3], phy))


def _request_peer_sca(hci, handle):
    status, _ = hci.command(OP_LE_REQUEST_PEER_SCA, struct.pack("<H", handle), allow_fail=True)
    if status != 0:
        raise HciError("LE Request Peer SCA returned %s" % status_text(status))
    body = wait_le_subevent(
        hci, LE_REQUEST_PEER_SCA_COMPLETE, handle, 2,
        predicate=lambda event: len(event) >= 5 and event[1] == 0,
    )
    if body is None:
        raise HciError("no successful LE Request Peer SCA Complete event")
    return body[4]


def _power_control(hci, handle):
    status, data = hci.command(
        OP_LE_ENHANCED_READ_TX_POWER,
        struct.pack("<HB", handle, PHY_1M),
        allow_fail=True,
    )
    if status != 0 or len(data) < 5:
        raise HciError("LE Enhanced Read Transmit Power Level returned %s" % status_text(status))

    status, _ = hci.command(
        OP_LE_SET_TX_POWER_REPORT_ENABLE,
        struct.pack("<HBB", handle, 1, 1),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Set Transmit Power Reporting Enable returned %s" % status_text(status))

    status, _ = hci.command(
        OP_LE_READ_REMOTE_TX_POWER,
        struct.pack("<HB", handle, PHY_1M),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Read Remote Transmit Power Level returned %s" % status_text(status))

    body = wait_le_subevent(
        hci, LE_TRANSMIT_POWER_REPORTING, handle, 2,
        predicate=lambda event: len(event) >= 8 and event[1] == 0,
    )
    if body is None:
        raise HciError("no successful LE Transmit Power Reporting event")


def _path_loss_monitor(hci, handle, timeout=3.0):
    params = struct.pack("<HBBBBH", handle, 70, 10, 50, 10, 5)
    status, _ = hci.command(OP_LE_SET_PATH_LOSS_PARAMS, params, allow_fail=True)
    if status != 0:
        raise HciError("LE Set Path Loss Reporting Parameters returned %s" % status_text(status))
    status, _ = hci.command(OP_LE_SET_PATH_LOSS_ENABLE,
                            struct.pack("<HB", handle, 1), allow_fail=True)
    if status != 0:
        raise HciError("LE Set Path Loss Reporting Enable returned %s" % status_text(status))
    body = wait_le_subevent(
        hci, LE_PATH_LOSS_THRESHOLD, handle, 1,
        predicate=lambda event: len(event) >= 5,
        timeout=timeout,
    )
    hci.command(OP_LE_SET_PATH_LOSS_ENABLE,
                struct.pack("<HB", handle, 0), allow_fail=True)
    return body is not None


def run_connected_feature_phase(book, label, central, peripheral):
    """Exercise procedures that require a live ACL connection."""
    handles = None
    try:
        peripheral_id, peripheral_type, _ = prepare_controller(peripheral)
        _, central_type, _ = prepare_controller(central)
        start_legacy_advertising(peripheral, peripheral_id, peripheral_type, True)
        start_legacy_connection(central, central_type, peripheral_id, peripheral_type)
        central_handle, peripheral_handle = wait_acl_pair(central, peripheral)
        handles = central_handle, peripheral_handle
        book.passed("ACL roles", label,
                    "central 0x%04X / peripheral 0x%04X" % handles)

        marker = (label + " central->peripheral").encode("ascii")
        central.send_acl(central_handle, TEST_CID, marker)
        if wait_acl_marker(peripheral, peripheral_handle, TEST_CID, marker):
            book.passed("ACL data", label + " central -> peripheral")
        else:
            book.failed("ACL data", label + " central -> peripheral", "marker not received")

        marker = (label + " peripheral->central").encode("ascii")
        peripheral.send_acl(peripheral_handle, TEST_CID, marker)
        if wait_acl_marker(central, central_handle, TEST_CID, marker):
            book.passed("ACL data", label + " peripheral -> central")
        else:
            book.failed("ACL data", label + " peripheral -> central", "marker not received")

        for side, hci, handle in (("Central", central, central_handle),
                                  ("Peripheral", peripheral, peripheral_handle)):
            try:
                rssi = _read_rssi(hci, handle)
                book.passed("Connection", "%s %s RSSI" % (label, side), "%d dBm" % rssi)
            except HciError as err:
                book.failed("Connection", "%s %s RSSI" % (label, side), str(err))

            try:
                features = _read_remote_features(hci, handle)
                book.passed("Connection", "%s %s remote features" % (label, side), features.hex(" "))
            except HciError as err:
                book.failed("Connection", "%s %s remote features" % (label, side), str(err))

        try:
            tx_octets, rx_octets = _set_data_length(central, central_handle)
            book.passed("DLE", label, "tx %u / rx %u octets" % (tx_octets, rx_octets))
        except HciError as err:
            book.failed("DLE", label, str(err))

        try:
            _set_phy(central, central_handle, PHY_2M)
            book.passed("PHY", label + " 2M")
            _set_phy(peripheral, peripheral_handle, PHY_1M)
            book.passed("PHY", label + " return to 1M")
        except HciError as err:
            book.failed("PHY", label, str(err))

        for side, hci, handle in (("Central", central, central_handle),
                                  ("Peripheral", peripheral, peripheral_handle)):
            try:
                sca = _request_peer_sca(hci, handle)
                book.passed("SCA", "%s %s request" % (label, side), "peer SCA code %u" % sca)
            except HciError as err:
                book.failed("SCA", "%s %s request" % (label, side), str(err))

            try:
                _power_control(hci, handle)
                book.passed("Power control", "%s %s" % (label, side))
            except HciError as err:
                book.failed("Power control", "%s %s" % (label, side), str(err))

        try:
            if _path_loss_monitor(central, central_handle):
                book.passed("Path loss", label + " threshold event")
            else:
                book.incomplete("Path loss", label + " threshold event",
                                "configuration accepted but no threshold crossing observed")
        except HciError as err:
            book.failed("Path loss", label, str(err))

    except (HciError, HciGone) as err:
        book.failed("Connection", label + " setup", str(err))
    finally:
        if handles is not None:
            disconnect_acl_pair(central, peripheral, handles[0], handles[1])


def run_subrate_phase(book, label, central, peripheral):
    """Enable host support before connecting, then complete a subrate request."""
    handles = None
    prepared = []
    try:
        peripheral_id, peripheral_type, _ = prepare_controller(peripheral)
        _, central_type, _ = prepare_controller(central)

        for side, hci in (("central", central), ("peripheral", peripheral)):
            status, _ = hci.command(
                OP_LE_SET_HOST_FEATURE,
                bytes([SUBRATING_HOST_SUPPORT_BIT, 1]),
                allow_fail=True,
            )
            if status != 0:
                raise HciError("%s LE Set Host Feature returned %s" % (side, status_text(status)))
            prepared.append(hci)

        start_legacy_advertising(peripheral, peripheral_id, peripheral_type, True)
        start_legacy_connection(central, central_type, peripheral_id, peripheral_type)
        central_handle, peripheral_handle = wait_acl_pair(central, peripheral)
        handles = central_handle, peripheral_handle

        payload = struct.pack("<HHHHHH", central_handle, 1, 1, 0, 0, 300)
        status, _ = central.command(OP_LE_SUBRATE_REQUEST, payload, allow_fail=True)
        if status != 0:
            raise HciError("LE Subrate Request returned %s" % status_text(status))

        body = wait_le_subevent(
            central, LE_SUBRATE_CHANGE, central_handle, 2,
            predicate=lambda event: len(event) >= 12 and event[1] == 0,
        )
        if body is None:
            raise HciError("no successful LE Subrate Change event")
        factor = struct.unpack("<H", body[4:6])[0]
        book.passed("Connection subrating", label, "factor %u" % factor)

    except (HciError, HciGone) as err:
        book.failed("Connection subrating", label, str(err))
    finally:
        if handles is not None:
            disconnect_acl_pair(central, peripheral, handles[0], handles[1])
        for hci in prepared:
            try:
                hci.command(OP_LE_SET_HOST_FEATURE,
                            bytes([SUBRATING_HOST_SUPPORT_BIT, 0]),
                            allow_fail=True)
            except (HciError, HciGone):
                pass

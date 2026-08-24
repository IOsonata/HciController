#!/usr/bin/env python3
"""Corrections for stateful release procedures built on connected_features."""

import struct
import time

from . import connected_features as _base
from .hci_pair import (
    disconnect_acl_pair,
    prepare_controller,
    start_legacy_advertising,
    start_legacy_connection,
    wait_acl_pair,
)

H4_ACL = _base.H4_ACL
HciError = _base.HciError
HciGone = _base.HciGone
status_text = _base.status_text

STATUS_CONTROLLER_BUSY = 0x3A
ACL_PB_CONTINUING = 0x01
ACL_PB_FIRST_AUTO_FLUSHABLE = 0x02
ACL_PB_COMPLETE = 0x03
PHY_CODED = 0x04
PHY_ALL = _base.PHY_1M | _base.PHY_2M | PHY_CODED
PHY_OPTION_NO_PREFERENCE = 0x0000
PHY_OPTION_CODED_S2 = 0x0001
PHY_OPTION_CODED_S8 = 0x0002


def wait_acl_marker(hci, expected_handle, expected_cid, expected_payload,
                    timeout=5.0):
    """Wait for an L2CAP marker while reassembling HCI ACL fragments."""
    deferred = []
    partial = None
    deadline = time.time() + timeout

    def finish_partial():
        nonlocal partial
        if partial is None or len(partial["data"]) < partial["expected_total"]:
            return None

        raw_packets = partial["packets"]
        complete = partial["data"][:partial["expected_total"]]
        partial = None

        if len(complete) >= 4:
            l2cap_len, cid = struct.unpack("<HH", complete[:4])
            payload = complete[4:4 + l2cap_len]
            if cid == expected_cid and payload == expected_payload:
                return True

        deferred.extend(raw_packets)
        return False

    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue

        kind, _, body = packet
        if kind != H4_ACL or len(body) < 4:
            deferred.append(packet)
            continue

        handle_flags, packet_len = struct.unpack("<HH", body[:4])
        handle = handle_flags & 0x0FFF
        pb = (handle_flags >> 12) & 0x03
        fragment = body[4:4 + packet_len]

        if handle != expected_handle or packet_len != len(body) - 4:
            deferred.append(packet)
            continue

        if pb in (0x00, ACL_PB_FIRST_AUTO_FLUSHABLE, ACL_PB_COMPLETE):
            if partial is not None:
                deferred.extend(partial["packets"])
                partial = None
            if len(fragment) < 4:
                deferred.append(packet)
                continue

            l2cap_len, _ = struct.unpack("<HH", fragment[:4])
            partial = {
                "expected_total": 4 + l2cap_len,
                "data": fragment,
                "packets": [packet],
            }
            if finish_partial() is True:
                _base._restore(hci, deferred)
                return True
            continue

        if pb == ACL_PB_CONTINUING and partial is not None:
            partial["data"] += fragment
            partial["packets"].append(packet)
            if finish_partial() is True:
                _base._restore(hci, deferred)
                return True
            continue

        deferred.append(packet)

    if partial is not None:
        deferred.extend(partial["packets"])
    _base._restore(hci, deferred)
    return False


def _command_status_retry(hci, opcode, payload, retries=10, delay=0.1):
    """Retry only Controller Busy; every other status is final."""
    for attempt in range(retries + 1):
        status, data = hci.command(opcode, payload, allow_fail=True)
        if status != STATUS_CONTROLLER_BUSY or attempt == retries:
            return status, data
        time.sleep(delay)
    return STATUS_CONTROLLER_BUSY, b""


def _power_control(hci, handle):
    """Exercise local/remote power reads without overlapping LL procedures."""
    status, data = hci.command(
        _base.OP_LE_ENHANCED_READ_TX_POWER,
        struct.pack("<HB", handle, _base.PHY_1M),
        allow_fail=True,
    )
    if status != 0 or len(data) < 5:
        raise HciError(
            "LE Enhanced Read Transmit Power Level returned %s"
            % status_text(status)
        )

    status, _ = _command_status_retry(
        hci,
        _base.OP_LE_READ_REMOTE_TX_POWER,
        struct.pack("<HB", handle, _base.PHY_1M),
    )
    if status != 0:
        raise HciError(
            "LE Read Remote Transmit Power Level returned %s"
            % status_text(status)
        )

    body = _base.wait_le_subevent(
        hci,
        _base.LE_TRANSMIT_POWER_REPORTING,
        handle,
        2,
        predicate=lambda event: len(event) >= 8 and event[1] == 0,
    )
    if body is None:
        raise HciError("no successful LE Transmit Power Reporting event")

    status, _ = hci.command(
        _base.OP_LE_SET_TX_POWER_REPORT_ENABLE,
        struct.pack("<HBB", handle, 1, 1),
        allow_fail=True,
    )
    if status != 0:
        raise HciError(
            "LE Set Transmit Power Reporting Enable returned %s"
            % status_text(status)
        )

    hci.command(
        _base.OP_LE_SET_TX_POWER_REPORT_ENABLE,
        struct.pack("<HBB", handle, 0, 0),
        allow_fail=True,
    )


def _request_phy(hci, handle, phy, expected_phy=None,
                 phy_options=PHY_OPTION_NO_PREFERENCE):
    """Request a PHY preference and return the completed TX/RX PHYs."""
    payload = struct.pack("<HBBBH", handle, 0, phy, phy, phy_options)
    status, _ = _command_status_retry(hci, _base.OP_LE_SET_PHY, payload)
    if status != 0:
        raise HciError("LE Set PHY returned %s" % status_text(status))

    body = _base.wait_le_subevent(
        hci,
        _base.LE_PHY_UPDATE_COMPLETE,
        handle,
        2,
        predicate=lambda event: len(event) >= 6 and event[1] == 0,
    )
    if body is None:
        raise HciError("no successful PHY Update Complete event")

    tx_phy, rx_phy = body[4], body[5]
    if expected_phy is not None and (
        tx_phy != expected_phy or rx_phy != expected_phy
    ):
        raise HciError(
            "PHY Update Complete reports tx %u rx %u, expected %u"
            % (tx_phy, rx_phy, expected_phy)
        )
    return tx_phy, rx_phy


def _wait_peer_phy(hci, handle, phy, timeout=5.0):
    body = _base.wait_le_subevent(
        hci,
        _base.LE_PHY_UPDATE_COMPLETE,
        handle,
        2,
        predicate=lambda event: (
            len(event) >= 6
            and event[1] == 0
            and event[4] == phy
            and event[5] == phy
        ),
        timeout=timeout,
    )
    if body is None:
        raise HciError("peer did not report PHY %u update" % phy)


def _read_phy_exact(hci, handle, phy):
    status, data = hci.command(
        _base.OP_LE_READ_PHY, struct.pack("<H", handle), allow_fail=True
    )
    if status != 0 or len(data) < 4:
        raise HciError("LE Read PHY failed")
    if data[2] != phy or data[3] != phy:
        raise HciError(
            "LE Read PHY reports tx %u rx %u, expected %u"
            % (data[2], data[3], phy)
        )


def _verify_acl_on_phy(central, peripheral,
                       central_handle, peripheral_handle, marker_name):
    """Prove bidirectional ACL transfer while the requested PHY is active."""
    marker = (marker_name + " central->peripheral").encode("ascii")
    central.send_acl(central_handle, _base.TEST_CID, marker)
    if not wait_acl_marker(
        peripheral, peripheral_handle, _base.TEST_CID, marker
    ):
        raise HciError(
            "%s central -> peripheral ACL marker not received" % marker_name
        )

    marker = (marker_name + " peripheral->central").encode("ascii")
    peripheral.send_acl(peripheral_handle, _base.TEST_CID, marker)
    if not wait_acl_marker(
        central, central_handle, _base.TEST_CID, marker
    ):
        raise HciError(
            "%s peripheral -> central ACL marker not received" % marker_name
        )


def _run_coded_phy(book, label, central, peripheral,
                   central_handle, peripheral_handle,
                   phy_options, option_name):
    """Switch to Coded PHY, prove OTA ACL traffic, then restore 1M."""
    _request_phy(
        central,
        central_handle,
        PHY_CODED,
        expected_phy=PHY_CODED,
        phy_options=phy_options,
    )
    _wait_peer_phy(peripheral, peripheral_handle, PHY_CODED)
    _read_phy_exact(central, central_handle, PHY_CODED)
    _read_phy_exact(peripheral, peripheral_handle, PHY_CODED)

    marker_name = "%s Coded %s" % (label, option_name)
    _verify_acl_on_phy(
        central,
        peripheral,
        central_handle,
        peripheral_handle,
        marker_name,
    )
    book.passed(
        "PHY",
        marker_name,
        "both controllers report Coded + bidirectional ACL",
    )

    _request_phy(
        central,
        central_handle,
        _base.PHY_1M,
        expected_phy=_base.PHY_1M,
    )
    _wait_peer_phy(peripheral, peripheral_handle, _base.PHY_1M)
    _read_phy_exact(central, central_handle, _base.PHY_1M)
    _read_phy_exact(peripheral, peripheral_handle, _base.PHY_1M)


def _run_phy_round_trip(book, label, central, peripheral,
                        central_handle, peripheral_handle):
    """Exercise 2M and Coded PHY updates, then leave the link on 1M."""
    _request_phy(
        peripheral, peripheral_handle, _base.PHY_2M,
        expected_phy=_base.PHY_2M,
    )
    _wait_peer_phy(central, central_handle, _base.PHY_2M)
    book.passed("PHY", label + " peripheral request 2M")

    # HCI LE Set PHY stores a local Host preference. The Central first asks for
    # 1M while the Peripheral still prefers 2M-only. Then the Peripheral is
    # changed to an all-PHY preference. The only PHY overlapping the Central's
    # 1M-only preference is 1M, so this both restores 1M and leaves the peer
    # permissive for the following Coded-PHY requests.
    _request_phy(central, central_handle, _base.PHY_1M)
    _request_phy(
        peripheral,
        peripheral_handle,
        PHY_ALL,
        expected_phy=_base.PHY_1M,
    )
    _wait_peer_phy(central, central_handle, _base.PHY_1M)
    _read_phy_exact(central, central_handle, _base.PHY_1M)
    _read_phy_exact(peripheral, peripheral_handle, _base.PHY_1M)
    book.passed("PHY", label + " return to 1M")

    _run_coded_phy(
        book,
        label,
        central,
        peripheral,
        central_handle,
        peripheral_handle,
        PHY_OPTION_CODED_S2,
        "S=2 preference",
    )
    _run_coded_phy(
        book,
        label,
        central,
        peripheral,
        central_handle,
        peripheral_handle,
        PHY_OPTION_CODED_S8,
        "S=8 preference",
    )


def run_connected_feature_phase(book, label, central, peripheral):
    """Run connected feature checks with correct ACL/PHY/power sequencing."""
    handles = None
    try:
        peripheral_id, peripheral_type, _ = prepare_controller(peripheral)
        _, central_type, _ = prepare_controller(central)
        start_legacy_advertising(
            peripheral, peripheral_id, peripheral_type, True
        )
        start_legacy_connection(
            central, central_type, peripheral_id, peripheral_type
        )
        central_handle, peripheral_handle = wait_acl_pair(central, peripheral)
        handles = central_handle, peripheral_handle
        book.passed(
            "ACL roles",
            label,
            "central 0x%04X / peripheral 0x%04X" % handles,
        )

        marker = (label + " central->peripheral").encode("ascii")
        central.send_acl(central_handle, _base.TEST_CID, marker)
        if wait_acl_marker(
            peripheral, peripheral_handle, _base.TEST_CID, marker
        ):
            book.passed("ACL data", label + " central -> peripheral")
        else:
            book.failed(
                "ACL data",
                label + " central -> peripheral",
                "marker not received",
            )

        marker = (label + " peripheral->central").encode("ascii")
        peripheral.send_acl(peripheral_handle, _base.TEST_CID, marker)
        if wait_acl_marker(
            central, central_handle, _base.TEST_CID, marker
        ):
            book.passed("ACL data", label + " peripheral -> central")
        else:
            book.failed(
                "ACL data",
                label + " peripheral -> central",
                "marker not received",
            )

        for side, hci, handle in (
            ("Central", central, central_handle),
            ("Peripheral", peripheral, peripheral_handle),
        ):
            try:
                rssi = _base._read_rssi(hci, handle)
                book.passed(
                    "Connection",
                    "%s %s RSSI" % (label, side),
                    "%d dBm" % rssi,
                )
            except HciError as err:
                book.failed(
                    "Connection", "%s %s RSSI" % (label, side), str(err)
                )

            try:
                features = _base._read_remote_features(hci, handle)
                book.passed(
                    "Connection",
                    "%s %s remote features" % (label, side),
                    features.hex(" "),
                )
            except HciError as err:
                book.failed(
                    "Connection",
                    "%s %s remote features" % (label, side),
                    str(err),
                )

        try:
            tx_octets, rx_octets = _base._set_data_length(
                central, central_handle
            )
            book.passed(
                "DLE",
                label,
                "tx %u / rx %u octets" % (tx_octets, rx_octets),
            )
        except HciError as err:
            book.failed("DLE", label, str(err))

        try:
            _run_phy_round_trip(
                book, label, central, peripheral,
                central_handle, peripheral_handle,
            )
        except HciError as err:
            book.failed("PHY", label, str(err))

        for side, hci, handle in (
            ("Central", central, central_handle),
            ("Peripheral", peripheral, peripheral_handle),
        ):
            try:
                sca = _base._request_peer_sca(hci, handle)
                book.passed(
                    "SCA",
                    "%s %s request" % (label, side),
                    "peer SCA code %u" % sca,
                )
            except HciError as err:
                book.failed(
                    "SCA", "%s %s request" % (label, side), str(err)
                )

            try:
                _power_control(hci, handle)
                book.passed("Power control", "%s %s" % (label, side))
            except HciError as err:
                book.failed(
                    "Power control", "%s %s" % (label, side), str(err)
                )

        try:
            threshold_seen = _base._path_loss_monitor(
                central, central_handle
            )
            book.passed("Path loss", label + " configuration")
            if threshold_seen:
                book.passed("Path loss", label + " threshold event")
            else:
                book.incomplete(
                    "Path loss",
                    label + " threshold event",
                    "no path-loss zone crossing was induced",
                )
        except HciError as err:
            book.failed("Path loss", label, str(err))

    except (HciError, HciGone) as err:
        book.failed("Connection", label + " setup", str(err))
    finally:
        if handles is not None:
            disconnect_acl_pair(
                central, peripheral, handles[0], handles[1]
            )


run_subrate_phase = _base.run_subrate_phase

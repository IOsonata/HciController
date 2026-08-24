#!/usr/bin/env python3
"""Positive two-controller Core 6.0/6.2 connected procedures."""

import struct

from . import connected_features as _base
from .hci_pair import (
    disconnect_acl_pair,
    prepare_controller,
    start_legacy_advertising,
    start_legacy_connection,
    wait_acl_pair,
)

HciError = _base.HciError
HciGone = _base.HciGone
status_text = _base.status_text

OP_LE_READ_ALL_LOCAL_FEATURES = 0x2087
OP_LE_READ_ALL_REMOTE_FEATURES = 0x2088
OP_LE_SET_HOST_FEATURE_V2 = 0x2097
OP_LE_FRAME_SPACE_UPDATE = 0x209D
OP_LE_CONNECTION_RATE_REQUEST = 0x20A1
OP_LE_SET_DEFAULT_RATE_PARAMETERS = 0x20A2

LE_READ_ALL_REMOTE_FEATURES_COMPLETE = 0x2B
LE_FRAME_SPACE_UPDATE_COMPLETE = 0x35
LE_CONNECTION_RATE_CHANGE = 0x37

LE_FEATURE_PAGE_MAX = 10
LE_REMOTE_FEATURE_PAGES_REQUESTED = 1
SCI_HOST_SUPPORT_BIT = 73

FRAME_SPACE_1M = 200
FRAME_SPACE_DEFAULT = 150
FRAME_SPACE_PHY_1M = 0x01
FRAME_SPACE_PHY_2M = 0x02
FRAME_SPACE_ACL_IFS_CP = 0x0001
FRAME_SPACE_ACL_IFS_PC = 0x0002
FRAME_SPACE_ACL_IFS = FRAME_SPACE_ACL_IFS_CP | FRAME_SPACE_ACL_IFS_PC

# nRF52 SDC reports 750 us as its minimum supported SCI. SCI units are 125 us.
SCI_INTERVAL = 6
SCI_REQUEST_MAX_INTERVAL = 80  # 10 ms, same initial interval used by Nordic's SCI sample.
SCI_SUBRATE = 1
SCI_MAX_LATENCY = 0
SCI_CONTINUATION = 0
SCI_SUPERVISION_TIMEOUT = 400
SCI_MIN_CE_LEN = 0x0001
SCI_MAX_CE_LEN = 0x3E7F


def _read_all_local_features(hci):
    status, data = hci.command(OP_LE_READ_ALL_LOCAL_FEATURES, allow_fail=True)
    if status != 0:
        raise HciError("LE Read All Local Supported Features returned %s"
                       % status_text(status))
    if len(data) != 249:
        raise HciError("LE Read All Local Supported Features returned %d bytes, expected 249"
                       % len(data))
    return data[0], data[1:]


def _valid_feature_bytes(max_page):
    if max_page > LE_FEATURE_PAGE_MAX:
        raise HciError("feature Max_Page %u exceeds Core maximum %u"
                       % (max_page, LE_FEATURE_PAGE_MAX))
    return 8 + (24 * max_page)


def _read_all_remote_features(hci, handle, page0_expected):
    # Pages_Requested is a count of extended feature pages, not a maximum page
    # number. One page is sufficient to prove the extended-feature procedure
    # and matches the positive command probe already validated on this SDC.
    payload = struct.pack("<HB", handle, LE_REMOTE_FEATURE_PAGES_REQUESTED)
    status, _ = hci.command(
        OP_LE_READ_ALL_REMOTE_FEATURES, payload, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Read All Remote Features returned %s"
                       % status_text(status))

    body = _base.wait_le_subevent(
        hci,
        LE_READ_ALL_REMOTE_FEATURES_COMPLETE,
        handle,
        2,
        predicate=lambda event: len(event) >= 4,
        timeout=8.0,
    )
    if body is None:
        raise HciError("no LE Read All Remote Features Complete event")
    if body[1] != 0:
        raise HciError("LE Read All Remote Features Complete returned %s"
                       % status_text(body[1]))
    if len(body) < 254:
        raise HciError(
            "LE Read All Remote Features Complete returned %d bytes, expected at least 254"
            % len(body)
        )

    max_remote_page = body[4]
    max_valid_page = body[5]
    features = body[6:254]
    if max_remote_page > LE_FEATURE_PAGE_MAX:
        raise HciError("remote Max_Page %u exceeds Core maximum" % max_remote_page)
    if max_valid_page > max_remote_page:
        raise HciError("Max_Valid_Page %u exceeds Max_Remote_Page %u"
                       % (max_valid_page, max_remote_page))
    if max_valid_page < 1:
        raise HciError("extended feature exchange did not retrieve page 1")
    if features[:8] != page0_expected:
        raise HciError(
            "all-remote page 0 %s differs from LE Read Remote Features Page 0 %s"
            % (features[:8].hex(" "), page0_expected.hex(" "))
        )
    return max_remote_page, max_valid_page, features


def _wait_frame_space(hci, handle, frame_space, expected_initiator):
    def matches(event):
        if len(event) < 10 or event[1] != 0:
            return False
        if event[4] != expected_initiator:
            return False
        got_space = struct.unpack("<H", event[5:7])[0]
        got_types = struct.unpack("<H", event[8:10])[0]
        return (
            got_space == frame_space
            and (event[7] & FRAME_SPACE_PHY_1M)
            and (got_types & FRAME_SPACE_ACL_IFS) == FRAME_SPACE_ACL_IFS
        )

    body = _base.wait_le_subevent(
        hci,
        LE_FRAME_SPACE_UPDATE_COMPLETE,
        handle,
        2,
        predicate=matches,
        timeout=8.0,
    )
    if body is None:
        raise HciError("no matching LE Frame Space Update Complete event")


def _frame_space_request(initiator, initiator_handle, peer, peer_handle,
                         frame_space):
    payload = struct.pack(
        "<HHHBH",
        initiator_handle,
        frame_space,
        frame_space,
        FRAME_SPACE_PHY_1M,
        FRAME_SPACE_ACL_IFS,
    )
    status, _ = initiator.command(
        OP_LE_FRAME_SPACE_UPDATE, payload, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Frame Space Update returned %s" % status_text(status))

    _wait_frame_space(initiator, initiator_handle, frame_space, 0)
    _wait_frame_space(peer, peer_handle, frame_space, 2)


def _wait_frame_space_range(hci, handle, frame_space_min, frame_space_max,
                            phys, expected_initiator):
    def matches(event):
        if len(event) < 10 or event[1] != 0:
            return False
        if event[4] != expected_initiator:
            return False
        got_space = struct.unpack("<H", event[5:7])[0]
        got_types = struct.unpack("<H", event[8:10])[0]
        return (
            frame_space_min <= got_space <= frame_space_max
            and (event[7] & phys)
            and (got_types & FRAME_SPACE_ACL_IFS) == FRAME_SPACE_ACL_IFS
        )

    body = _base.wait_le_subevent(
        hci,
        LE_FRAME_SPACE_UPDATE_COMPLETE,
        handle,
        2,
        predicate=matches,
        timeout=8.0,
    )
    if body is None:
        raise HciError("no matching LE Frame Space Update Complete event for SCI")
    return struct.unpack("<H", body[5:7])[0]


def _frame_space_range_request(initiator, initiator_handle, peer, peer_handle,
                               frame_space_min, frame_space_max, phys):
    payload = struct.pack(
        "<HHHBH",
        initiator_handle,
        frame_space_min,
        frame_space_max,
        phys,
        FRAME_SPACE_ACL_IFS,
    )
    status, _ = initiator.command(
        OP_LE_FRAME_SPACE_UPDATE, payload, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Frame Space Update for SCI returned %s"
                       % status_text(status))

    local_space = _wait_frame_space_range(
        initiator, initiator_handle, frame_space_min, frame_space_max,
        phys, 0,
    )
    peer_space = _wait_frame_space_range(
        peer, peer_handle, frame_space_min, frame_space_max,
        phys, 2,
    )
    if local_space != peer_space:
        raise HciError(
            "SCI frame-space result differs across peers: %u vs %u us"
            % (local_space, peer_space)
        )
    return local_space


def _set_sci_host_support(hci, enable):
    status, _ = hci.command(
        OP_LE_SET_HOST_FEATURE_V2,
        struct.pack("<HB", SCI_HOST_SUPPORT_BIT, 1 if enable else 0),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Set Host Feature v2 bit %u returned %s"
                       % (SCI_HOST_SUPPORT_BIT, status_text(status)))


def _rate_parameters(with_handle=None, interval_min=SCI_INTERVAL,
                     interval_max=SCI_INTERVAL):
    values = (
        interval_min,
        interval_max,
        SCI_SUBRATE,
        SCI_SUBRATE,
        SCI_MAX_LATENCY,
        SCI_CONTINUATION,
        SCI_SUPERVISION_TIMEOUT,
        SCI_MIN_CE_LEN,
        SCI_MAX_CE_LEN,
    )
    if with_handle is None:
        return struct.pack("<9H", *values)
    return struct.pack("<H9H", with_handle, *values)


def _set_default_rate_parameters(hci):
    status, _ = hci.command(
        OP_LE_SET_DEFAULT_RATE_PARAMETERS,
        _rate_parameters(),
        allow_fail=True,
    )
    if status != 0:
        raise HciError("LE Set Default Rate Parameters returned %s"
                       % status_text(status))


def _wait_rate_change(hci, handle):
    def matches(event):
        if len(event) < 14 or event[1] != 0:
            return False
        interval, subrate, latency, continuation, timeout = struct.unpack(
            "<5H", event[4:14]
        )
        return (
            interval == SCI_INTERVAL
            and subrate == SCI_SUBRATE
            and latency == SCI_MAX_LATENCY
            and continuation == SCI_CONTINUATION
            and timeout == SCI_SUPERVISION_TIMEOUT
        )

    body = _base.wait_le_subevent(
        hci,
        LE_CONNECTION_RATE_CHANGE,
        handle,
        2,
        predicate=matches,
        timeout=8.0,
    )
    if body is None:
        raise HciError("no matching LE Connection Rate Change event")


def run_core_advanced_phase(book, label, central, peripheral):
    """Exercise Extended Feature Set, FSU, and SCI on one fresh ACL link."""
    handles = None
    sci_enabled = []
    steps = (
        ("Core 6.0", "LE Read All Remote Features completion"),
        ("Core 6.0", "Frame Space Update positive procedure"),
        ("Core 6.2", "Connection Rate / Shorter Connection Intervals"),
    )
    current_step = 0
    try:
        peripheral_id, peripheral_type, _ = prepare_controller(peripheral)
        _, central_type, _ = prepare_controller(central)

        # SCI Host Support affects link-layer feature negotiation and therefore
        # has to be enabled before the connection is created.
        for hci in (central, peripheral):
            _set_sci_host_support(hci, True)
            sci_enabled.append(hci)
            _set_default_rate_parameters(hci)

        central_local_max, _ = _read_all_local_features(central)
        peripheral_local_max, _ = _read_all_local_features(peripheral)

        start_legacy_advertising(peripheral, peripheral_id, peripheral_type, True)
        start_legacy_connection(central, central_type, peripheral_id, peripheral_type)
        central_handle, peripheral_handle = wait_acl_pair(central, peripheral)
        handles = central_handle, peripheral_handle

        central_page0 = _base._read_remote_features(central, central_handle)
        peripheral_page0 = _base._read_remote_features(peripheral, peripheral_handle)

        c_remote = _read_all_remote_features(
            central, central_handle, central_page0
        )
        p_remote = _read_all_remote_features(
            peripheral, peripheral_handle, peripheral_page0
        )

        if c_remote[0] != peripheral_local_max:
            raise HciError(
                "Central sees peer Max_Page %u, Peripheral reports local Max_Page %u"
                % (c_remote[0], peripheral_local_max)
            )
        if p_remote[0] != central_local_max:
            raise HciError(
                "Peripheral sees peer Max_Page %u, Central reports local Max_Page %u"
                % (p_remote[0], central_local_max)
            )
        common_valid = min(c_remote[1], p_remote[1])
        compare_len = _valid_feature_bytes(common_valid)
        if c_remote[2][:compare_len] != p_remote[2][:compare_len]:
            raise HciError("reciprocal all-remote feature pages differ")

        book.passed(
            "Core 6.0",
            "LE Read All Remote Features completion",
            "Max_Page %u / Max_Valid_Page %u" % (c_remote[0], c_remote[1]),
        )
        current_step = 1

        # Prove both ACL roles can initiate the symmetric FSU procedure. The
        # Peripheral restores the 1M ACL IFS after the Central first changes it.
        _frame_space_request(
            central, central_handle, peripheral, peripheral_handle,
            FRAME_SPACE_1M,
        )
        _frame_space_request(
            peripheral, peripheral_handle, central, central_handle,
            FRAME_SPACE_DEFAULT,
        )
        book.passed(
            "Core 6.0",
            "Frame Space Update positive procedure",
            "Central 200 us, Peripheral restore 150 us",
        )
        current_step = 2

        # nRF52's 750 us minimum requires LE 2M and a reduced ACL frame space.
        # Nordic's SCI sample performs these two updates before requesting the
        # shortest interval. Keep them inside the SCI row: the dedicated FSU
        # row above already proves symmetric Frame Space Update behavior.
        _base._set_phy(peripheral, peripheral_handle, _base.PHY_2M)
        sci_frame_space = _frame_space_range_request(
            peripheral,
            peripheral_handle,
            central,
            central_handle,
            0,
            FRAME_SPACE_DEFAULT,
            FRAME_SPACE_PHY_2M,
        )

        # The Peripheral proposes a range beginning at the common minimum. The
        # Central defaults constrain selection to 750 us. CE-length fields use
        # the Core-defined non-zero SCI bounds.
        status, _ = peripheral.command(
            OP_LE_CONNECTION_RATE_REQUEST,
            _rate_parameters(
                peripheral_handle,
                interval_min=SCI_INTERVAL,
                interval_max=SCI_REQUEST_MAX_INTERVAL,
            ),
            allow_fail=True,
        )
        if status != 0:
            raise HciError("LE Connection Rate Request returned %s"
                           % status_text(status))
        _wait_rate_change(peripheral, peripheral_handle)
        _wait_rate_change(central, central_handle)
        book.passed(
            "Core 6.2",
            "Connection Rate / Shorter Connection Intervals",
            "750 us, subrate 1, 2M PHY, %u us frame space"
            % sci_frame_space,
        )
        current_step = 3

    except (HciError, HciGone) as err:
        # Do not let a shared setup failure make later advertised rows vanish
        # from the release matrix. The procedure that actually failed is FAIL;
        # dependent procedures that were never reached remain explicitly
        # INCOMPLETE and therefore still block release.
        if current_step < len(steps):
            group, name = steps[current_step]
            book.failed(group, name, str(err))
            for group, name in steps[current_step + 1:]:
                book.incomplete(group, name, "not reached after prior Core procedure failure")
    finally:
        if handles is not None:
            disconnect_acl_pair(
                central, peripheral, handles[0], handles[1]
            )
        for hci in sci_enabled:
            try:
                _set_sci_host_support(hci, False)
            except (HciError, HciGone):
                pass
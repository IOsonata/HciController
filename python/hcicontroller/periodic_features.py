#!/usr/bin/env python3
"""Two-controller Periodic Advertising, PAST, and PAwR procedures."""

import struct
import threading
import time

from . import connected_features as _base
from .hci_pair import (
    disconnect_acl_pair,
    prepare_controller,
    wait_acl_pair,
)

H4_EVENT = _base.H4_EVENT
HciError = _base.HciError
HciGone = _base.HciGone
status_text = _base.status_text
EVT_LE_META = _base.EVT_LE_META
EVT_COMMAND_COMPLETE = 0x0E

OP_LE_SET_ADV_SET_RANDOM_ADDRESS = 0x2035
OP_LE_SET_EXT_ADV_PARAMS = 0x2036
OP_LE_SET_EXT_ADV_DATA = 0x2037
OP_LE_SET_EXT_ADV_ENABLE = 0x2039
OP_LE_SET_PERIODIC_ADV_PARAMS = 0x203E
OP_LE_SET_PERIODIC_ADV_DATA = 0x203F
OP_LE_SET_PERIODIC_ADV_ENABLE = 0x2040
OP_LE_SET_EXT_SCAN_PARAMS = 0x2041
OP_LE_SET_EXT_SCAN_ENABLE = 0x2042
OP_LE_EXT_CREATE_CONNECTION = 0x2043
OP_LE_PERIODIC_CREATE_SYNC = 0x2044
OP_LE_PERIODIC_CREATE_SYNC_CANCEL = 0x2045
OP_LE_PERIODIC_TERMINATE_SYNC = 0x2046
OP_LE_SET_PAST_PARAMS = 0x205C
OP_LE_PERIODIC_SET_INFO_TRANSFER = 0x205B
OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA = 0x2082
OP_LE_SET_PERIODIC_ADV_RESPONSE_DATA = 0x2083
OP_LE_SET_PERIODIC_SYNC_SUBEVENT = 0x2084
OP_LE_SET_PERIODIC_ADV_PARAMS_V2 = 0x2086
OP_VS_READ_COUNTERS = 0xFFF0

LE_EXTENDED_ADVERTISING_REPORT = 0x0D
LE_PERIODIC_SYNC_ESTABLISHED = 0x0E
LE_PERIODIC_ADV_REPORT = 0x0F
LE_PAST_RECEIVED = 0x18
LE_PERIODIC_SYNC_ESTABLISHED_V2 = 0x24
LE_PERIODIC_ADV_REPORT_V2 = 0x25
LE_PAST_RECEIVED_V2 = 0x26
LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST = 0x27
LE_PERIODIC_ADV_RESPONSE_REPORT = 0x28

ADV_HANDLE_PERIODIC = 0
ADV_HANDLE_CONNECTABLE = 1
ADV_SID_PERIODIC = 0
ADV_SID_CONNECTABLE = 1
PERIODIC_MARKER = b"HCI periodic OTA"
EXT_ADV_DISCOVERY_MARKER = b"HCI periodic source"
EXT_ADV_DISCOVERY_DATA = (
    b"\x02\x01\x06"
    + bytes([len(EXT_ADV_DISCOVERY_MARKER) + 1, 0x09])
    + EXT_ADV_DISCOVERY_MARKER
)
PAWR_NUM_SUBEVENTS = 2
PAWR_SUBEVENT_MARKER = b"HCI PAwR subevent"
PAWR_RESPONSE_MARKER = b"HCI PAwR response"
PAST_SERVICE_DATA = 0x4843


def _u24(value):
    return bytes((value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF))


def _command_ok(hci, opcode, payload=b"", name=None):
    status, data = hci.command(opcode, payload, allow_fail=True)
    if status != 0:
        raise HciError("%s returned %s"
                       % (name or "opcode 0x%04X" % opcode, status_text(status)))
    return data


def _pawr_diag_counters(hci):
    """Return the four version-5 PAwR routing counters, if the image has them."""
    status, data = hci.command(OP_VS_READ_COUNTERS, allow_fail=True)
    if status != 0 or len(data) < 1 + (38 * 4) or data[0] < 5:
        return None

    values = []
    for index in range(34, 38):
        at = 1 + index * 4
        values.append(struct.unpack("<I", data[at:at + 4])[0])
    return tuple(values)


def _is_command_complete_for(packet, opcode):
    kind, code, body = packet
    return (
        kind == H4_EVENT
        and code == EVT_COMMAND_COMPLETE
        and len(body) >= 3
        and struct.unpack("<H", body[1:3])[0] == opcode
    )


def _verify_pawr_response_completion(hci, before, after):
    """Reject duplicate 0x2083 completions and verify the firmware checkpoints."""
    duplicates = []
    kept = []
    for packet in hci.pending:
        if _is_command_complete_for(packet, OP_LE_SET_PERIODIC_ADV_RESPONSE_DATA):
            duplicates.append(packet)
        else:
            kept.append(packet)
    hci.pending = kept

    delta = None
    if before is not None and after is not None:
        delta = tuple((after[i] - before[i]) & 0xFFFFFFFF for i in range(4))

    if duplicates:
        detail = ""
        if delta is not None:
            detail = (
                "; PAwR counters candidate=%u handler=%u suppressed=%u "
                "sdc_complete=%u" % delta
            )
        raise HciError(
            "duplicate Command Complete for opcode 0x2083 (%u extra)%s"
            % (len(duplicates), detail)
        )

    if delta is not None and delta != (1, 1, 1, 1):
        raise HciError(
            "PAwR delayed completion path mismatch: candidate=%u handler=%u "
            "suppressed=%u sdc_complete=%u" % delta
        )


def _ext_adv_params(handle, own_addr_type, properties, sid):
    payload = bytes([handle])
    payload += struct.pack("<H", properties)
    payload += _u24(0x00A0) + _u24(0x00A0)
    payload += bytes([0x07, own_addr_type, 0x00])
    payload += bytes(6)
    payload += bytes([0x00, 0x7F, 0x01, 0x00, 0x01, sid, 0x00])
    if len(payload) != 25:
        raise AssertionError("extended advertising parameter length")
    return payload


def _configure_ext_set(hci, handle, own_addr_type, properties, sid, data=b""):
    _command_ok(
        hci,
        OP_LE_SET_EXT_ADV_PARAMS,
        _ext_adv_params(handle, own_addr_type, properties, sid),
        "LE Set Extended Advertising Parameters",
    )

    # Extended advertising has a random address per advertising set. Setting
    # the legacy/global random address is not sufficient when Own_Address_Type
    # is random. Release-1 HciController exposes a stable static random identity,
    # so read that identity again after the set exists and program it per set.
    if own_addr_type == 0x01:
        own_addr, identity_type, _ = hci.identity()
        if identity_type != 0x01 or len(own_addr) != 6:
            raise HciError("no usable random identity for extended advertising set")
        _command_ok(
            hci,
            OP_LE_SET_ADV_SET_RANDOM_ADDRESS,
            bytes([handle]) + own_addr,
            "LE Set Advertising Set Random Address",
        )

    if data:
        if len(data) > 251:
            raise HciError("extended advertising marker too large")
        payload = bytes([handle, 0x03, 0x01, len(data)]) + data
        _command_ok(
            hci,
            OP_LE_SET_EXT_ADV_DATA,
            payload,
            "LE Set Extended Advertising Data",
        )


def _ext_adv_enable(hci, handle, enable=True):
    if enable:
        payload = bytes([1, 1, handle]) + struct.pack("<H", 0) + bytes([0])
    else:
        # Num_Sets is zero when disabling all extended advertising.
        payload = bytes([0, 0])
    _command_ok(hci, OP_LE_SET_EXT_ADV_ENABLE, payload,
                "LE Set Extended Advertising Enable")


def _configure_periodic(hci, own_addr_type, marker=PERIODIC_MARKER):
    _configure_ext_set(
        hci, ADV_HANDLE_PERIODIC, own_addr_type, 0x0000,
        ADV_SID_PERIODIC, EXT_ADV_DISCOVERY_DATA,
    )
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_PARAMS,
        struct.pack("<BHHH", ADV_HANDLE_PERIODIC, 0x0060, 0x0060, 0),
        "LE Set Periodic Advertising Parameters",
    )
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_DATA,
        bytes([ADV_HANDLE_PERIODIC, 0x03, len(marker)]) + marker,
        "LE Set Periodic Advertising Data",
    )


def _start_periodic(hci):
    # Start the associated extended advertising set first. Nordic's periodic
    # advertising examples use this order before enabling the periodic train.
    _ext_adv_enable(hci, ADV_HANDLE_PERIODIC, True)
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_ENABLE,
        bytes([1, ADV_HANDLE_PERIODIC]),
        "LE Set Periodic Advertising Enable",
    )


def _stop_periodic(hci):
    for opcode, payload in (
        (OP_LE_SET_PERIODIC_ADV_ENABLE, bytes([0, ADV_HANDLE_PERIODIC])),
        (OP_LE_SET_EXT_ADV_ENABLE, bytes([0, 0])),
    ):
        try:
            hci.command(opcode, payload, allow_fail=True)
        except (HciError, HciGone):
            pass


def _set_sync_scan(hci, enable, allow_fail=False):
    payload = bytes([0x01 if enable else 0x00, 0x00])
    payload += struct.pack("<HH", 0, 0)
    if allow_fail:
        try:
            hci.command(OP_LE_SET_EXT_SCAN_ENABLE, payload, allow_fail=True)
        except (HciError, HciGone):
            pass
        return
    _command_ok(
        hci,
        OP_LE_SET_EXT_SCAN_ENABLE,
        payload,
        "LE Set Extended Scan Enable",
    )


def _cancel_pending_periodic_sync(hci):
    try:
        hci.command(
            OP_LE_PERIODIC_CREATE_SYNC_CANCEL,
            allow_fail=True,
        )
    except (HciError, HciGone):
        pass


def _extended_reports(body):
    # Vol 4 Part E 7.7.65.13. Keep the full address tuple and SID: those are
    # the identifiers LE Periodic Advertising Create Sync actually consumes.
    if len(body) < 2 or body[0] != LE_EXTENDED_ADVERTISING_REPORT:
        return []

    reports = []
    at = 2
    for _ in range(body[1]):
        if len(body) < at + 24:
            break
        addr_type = body[at + 2]
        addr = body[at + 3:at + 9]
        sid = body[at + 11]
        periodic_interval = struct.unpack("<H", body[at + 14:at + 16])[0]
        data_len = body[at + 23]
        if len(body) < at + 24 + data_len:
            break
        data = body[at + 24:at + 24 + data_len]
        reports.append((addr_type, addr, sid, periodic_interval, data))
        at += 24 + data_len
    return reports


def _periodic_source_matches(report, addr, addr_type, sid):
    report_type, report_addr, report_sid, periodic_interval, data = report
    return (
        report_type == addr_type
        and report_addr == addr
        and report_sid == sid
        and periodic_interval != 0
        and EXT_ADV_DISCOVERY_MARKER in data
    )


def _observe_periodic_advertiser(
        scanner, addr, addr_type, sid=ADV_SID_PERIODIC, timeout=5.0):
    # Multiple release benches can advertise the same discovery marker and SID
    # at the same time. The advertising set is explicitly programmed with the
    # local controller identity, so only that exact AdvA tuple belongs to this
    # test pair. A marker match from another controller must be ignored.
    _, own_addr_type, _ = scanner.identity()
    params = bytes([own_addr_type, 0x00, 0x01, 0x00])
    params += struct.pack("<HH", 0x0060, 0x0030)
    _command_ok(
        scanner,
        OP_LE_SET_EXT_SCAN_PARAMS,
        params,
        "LE Set Extended Scan Parameters",
    )
    _set_sync_scan(scanner, True)

    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = scanner.read_packet(
            min(0.2, max(0.0, deadline - time.time()))
        )
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        for report in _extended_reports(body):
            if _periodic_source_matches(report, addr, addr_type, sid):
                return report[:4]

    _set_sync_scan(scanner, False, allow_fail=True)
    raise HciError(
        "expected periodic source extended advertisement with SyncInfo was not observed"
    )


def _create_periodic_sync(scanner, addr, addr_type, sid=ADV_SID_PERIODIC):
    observed_type, observed_addr, observed_sid, _ = (
        _observe_periodic_advertiser(scanner, addr, addr_type, sid)
    )

    payload = bytes([0, observed_sid, observed_type]) + observed_addr
    payload += struct.pack("<HHB", 0, 0x0200, 0)
    status, _ = scanner.command(
        OP_LE_PERIODIC_CREATE_SYNC, payload, allow_fail=True
    )
    if status != 0:
        _set_sync_scan(scanner, False, allow_fail=True)
        raise HciError("LE Periodic Advertising Create Sync returned %s"
                       % status_text(status))


def _wait_sync_established(hci, timeout=10.0, require_v2=False,
                           keep_scan=False):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META and len(body) >= 4:
            if body[0] in (LE_PERIODIC_SYNC_ESTABLISHED,
                           LE_PERIODIC_SYNC_ESTABLISHED_V2):
                if require_v2 and body[0] != LE_PERIODIC_SYNC_ESTABLISHED_V2:
                    deferred.append(packet)
                    continue
                if body[1] != 0:
                    _set_sync_scan(hci, False, allow_fail=True)
                    _base._restore(hci, deferred)
                    raise HciError("Periodic Sync Established returned %s"
                                   % status_text(body[1]))
                handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
                if not keep_scan:
                    _set_sync_scan(hci, False, allow_fail=True)
                _base._restore(hci, deferred)
                return handle, body
        deferred.append(packet)

    _cancel_pending_periodic_sync(hci)
    _set_sync_scan(hci, False, allow_fail=True)
    _base._restore(hci, deferred)
    raise HciError("timed out waiting for Periodic Advertising Sync Established")


def _periodic_report_data(body):
    if not body:
        return None
    if body[0] == LE_PERIODIC_ADV_REPORT:
        if len(body) < 8:
            return None
        handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        length = body[7]
        if len(body) < 8 + length:
            return None
        return handle, None, None, body[8:8 + length]
    if body[0] == LE_PERIODIC_ADV_REPORT_V2:
        if len(body) < 11:
            return None
        handle = struct.unpack("<H", body[1:3])[0] & 0x0FFF
        event_counter = struct.unpack("<H", body[6:8])[0]
        subevent = body[8]
        length = body[10]
        if len(body) < 11 + length:
            return None
        return handle, event_counter, subevent, body[11:11 + length]
    return None


def _wait_periodic_report(hci, sync_handle, marker, timeout=8.0, require_v2=False):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META:
            parsed = _periodic_report_data(body)
            if parsed is not None and parsed[0] == sync_handle:
                if require_v2 and body[0] != LE_PERIODIC_ADV_REPORT_V2:
                    deferred.append(packet)
                    continue
                if marker in parsed[3]:
                    _base._restore(hci, deferred)
                    return parsed
        deferred.append(packet)
    _base._restore(hci, deferred)
    raise HciError("timed out waiting for periodic advertising marker")


def _terminate_sync(hci, handle):
    try:
        hci.command(
            OP_LE_PERIODIC_TERMINATE_SYNC,
            struct.pack("<H", handle),
            allow_fail=True,
        )
    except (HciError, HciGone):
        pass


def run_periodic_sync_phase(book, label, advertiser, scanner):
    sync_handle = None
    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)
        _configure_periodic(advertiser, adv_type)
        _start_periodic(advertiser)
        _create_periodic_sync(scanner, adv_id, adv_type)
        sync_handle, _ = _wait_sync_established(scanner)
        _wait_periodic_report(scanner, sync_handle, PERIODIC_MARKER)
        book.passed(
            "Periodic advertising",
            "Advertiser -> periodic sync",
            "%s sync 0x%04X + OTA marker" % (label, sync_handle),
        )
    except (HciError, HciGone) as err:
        book.failed("Periodic advertising", "Advertiser -> periodic sync", str(err))
    finally:
        if sync_handle is not None:
            _terminate_sync(scanner, sync_handle)
        _stop_periodic(advertiser)


def _start_extended_connection(central, central_type, peer_addr, peer_type):
    payload = bytes([0, central_type, peer_type]) + peer_addr + bytes([0x01])
    payload += struct.pack(
        "<8H", 0x0060, 0x0030, 0x0018, 0x0028, 0, 400, 0, 0
    )
    if len(payload) != 26:
        raise AssertionError("extended create connection parameter length")
    status, _ = central.command(
        OP_LE_EXT_CREATE_CONNECTION, payload, allow_fail=True
    )
    if status != 0:
        raise HciError("LE Extended Create Connection returned %s"
                       % status_text(status))


def _wait_past_received(hci, conn_handle, service_data, timeout=8.0):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META and len(body) >= 8:
            if body[0] in (LE_PAST_RECEIVED, LE_PAST_RECEIVED_V2):
                status = body[1]
                got_conn = struct.unpack("<H", body[2:4])[0] & 0x0FFF
                got_service = struct.unpack("<H", body[4:6])[0]
                sync_handle = struct.unpack("<H", body[6:8])[0] & 0x0FFF
                if got_conn == conn_handle and got_service == service_data:
                    _base._restore(hci, deferred)
                    if status != 0:
                        raise HciError("PAST Received returned %s"
                                       % status_text(status))
                    return sync_handle
        deferred.append(packet)
    _base._restore(hci, deferred)
    raise HciError("timed out waiting for LE Periodic Advertising Sync Transfer Received")


def run_past_phase(book, label, advertiser, receiver):
    handles = None
    transferred_sync = None
    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        _, receiver_type, _ = prepare_controller(receiver)

        _configure_periodic(advertiser, adv_type)
        _configure_ext_set(
            advertiser,
            ADV_HANDLE_CONNECTABLE,
            adv_type,
            0x0001,
            ADV_SID_CONNECTABLE,
            b"\x02\x01\x06",
        )
        _start_periodic(advertiser)
        _ext_adv_enable(advertiser, ADV_HANDLE_CONNECTABLE, True)

        _start_extended_connection(receiver, receiver_type, adv_id, adv_type)
        receiver_handle, advertiser_handle = wait_acl_pair(receiver, advertiser)
        handles = receiver_handle, advertiser_handle

        past_params = struct.pack(
            "<HBHHB", receiver_handle, 0x02, 0, 0x0200, 0
        )
        _command_ok(
            receiver,
            OP_LE_SET_PAST_PARAMS,
            past_params,
            "LE Set Periodic Advertising Sync Transfer Parameters",
        )

        # This minimal Host does not run the usual automatic feature exchange
        # after connection establishment. Make the sender learn the peer's
        # PAST Recipient bit before asking the Controller to transfer SyncInfo.
        features = _base._read_remote_features(advertiser, advertiser_handle)
        if not (int.from_bytes(features, "little") & (1 << 25)):
            raise HciError("peer does not advertise PAST Recipient")

        transfer = struct.pack(
            "<HHB", advertiser_handle, PAST_SERVICE_DATA, ADV_HANDLE_PERIODIC
        )
        _command_ok(
            advertiser,
            OP_LE_PERIODIC_SET_INFO_TRANSFER,
            transfer,
            "LE Periodic Advertising Set Info Transfer",
        )

        transferred_sync = _wait_past_received(
            receiver, receiver_handle, PAST_SERVICE_DATA
        )
        _wait_periodic_report(
            receiver, transferred_sync, PERIODIC_MARKER, timeout=10.0
        )
        book.passed(
            "PAST",
            "sender and receiver",
            "%s set-info transfer -> sync 0x%04X + OTA marker"
            % (label, transferred_sync),
        )
    except (HciError, HciGone) as err:
        book.failed("PAST", "sender and receiver", str(err))
    finally:
        if transferred_sync is not None:
            _terminate_sync(receiver, transferred_sync)
        if handles is not None:
            disconnect_acl_pair(receiver, advertiser, handles[0], handles[1])
        try:
            _ext_adv_enable(advertiser, ADV_HANDLE_CONNECTABLE, False)
        except (HciError, HciGone):
            pass
        _stop_periodic(advertiser)


def _configure_pawr(hci, own_addr_type):
    _configure_ext_set(
        hci, ADV_HANDLE_PERIODIC, own_addr_type, 0x0000,
        ADV_SID_PERIODIC, EXT_ADV_DISCOVERY_DATA,
    )
    payload = struct.pack(
        "<BHHHBBBBB",
        ADV_HANDLE_PERIODIC,
        0x00FF,
        0x00FF,
        0,
        PAWR_NUM_SUBEVENTS,
        0x60,   # 120 ms; two subevents still fit in the 318.75 ms period
        0x40,   # 80 ms response delay gives the HCI Host time to reply
        0x50,
        1,      # one response slot; spacing is ignored for a single slot
    )
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_PARAMS_V2,
        payload,
        "LE Set Periodic Advertising Parameters v2",
    )


def _wait_pawr_data_request(hci, timeout=8.0):
    body = _base.wait_le_subevent(
        hci,
        LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST,
        predicate=lambda event: (
            len(event) >= 4 and event[1] == ADV_HANDLE_PERIODIC
            and event[3] > 0
        ),
        timeout=timeout,
    )
    if body is None:
        raise HciError("no LE Periodic Advertising Subevent Data Request event")
    return body[2], body[3]


def _set_requested_pawr_subevent_data(hci, start, count):
    # Fill every subevent the Controller asked the Host to prepare. The request
    # sequence wraps from Num_Subevents - 1 back to zero; treating it as a
    # monotonically increasing byte produces an invalid subevent number.
    if start >= PAWR_NUM_SUBEVENTS:
        raise HciError("PAwR data request start exceeds configured subevents")
    if count <= 0 or count > PAWR_NUM_SUBEVENTS:
        raise HciError("PAwR data request count exceeds configured subevents")

    marker = PAWR_SUBEVENT_MARKER
    elements = []
    for offset in range(count):
        subevent = (start + offset) % PAWR_NUM_SUBEVENTS
        elements.append(bytes([subevent, 0, 1, len(marker)]) + marker)

    payload = bytes([ADV_HANDLE_PERIODIC, count]) + b"".join(elements)
    if len(payload) > 0xFF:
        raise HciError("PAwR subevent data does not fit one HCI command")

    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_SUBEVENT_DATA,
        payload,
        "LE Set Periodic Advertising Subevent Data",
    )
    return start


def _start_pawr_data_service(hci):
    """Service advertiser 0x27 requests independently of scanner timing."""
    service = {
        "stop": threading.Event(),
        "deferred": [],
        "errors": [],
        "finished": False,
    }

    def worker():
        try:
            while not service["stop"].is_set():
                packet = hci.read_packet(0.1)
                if packet is None:
                    continue
                kind, code, body = packet
                if (kind == H4_EVENT and code == EVT_LE_META
                        and len(body) >= 4
                        and body[0] == LE_PERIODIC_ADV_SUBEVENT_DATA_REQUEST
                        and body[1] == ADV_HANDLE_PERIODIC
                        and body[3] > 0):
                    _set_requested_pawr_subevent_data(
                        hci, body[2], body[3]
                    )
                    continue
                service["deferred"].append(packet)
        except (HciError, HciGone) as err:
            service["errors"].append(err)
            service["stop"].set()

    service["thread"] = threading.Thread(
        target=worker, name="pawr-data-service", daemon=True
    )
    service["thread"].start()
    return service


def _finish_pawr_data_service(hci, service):
    if service is None or service["finished"]:
        return

    service["stop"].set()
    service["thread"].join(3.5)
    if service["thread"].is_alive():
        raise HciError("PAwR data service did not stop")

    _base._restore(hci, service["deferred"])
    service["deferred"].clear()
    service["finished"] = True

    if service["errors"]:
        raise service["errors"][0]


def _discard_pre_pawr_sync_reports(hci, sync_handle):
    """Remove same-sync periodic reports queued before 0x2084 completed."""
    kept = []
    for packet in hci.pending:
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META:
            parsed = _periodic_report_data(body)
            if (
                    parsed is not None
                    and parsed[0] == sync_handle
                    and body[0] in (
                        LE_PERIODIC_ADV_REPORT,
                        LE_PERIODIC_ADV_REPORT_V2,
                    )):
                continue
        kept.append(packet)
    hci.pending = kept


def _set_pawr_sync_subevent(hci, sync_handle, subevent):
    payload = struct.pack("<HHB", sync_handle, 0, 1) + bytes([subevent])
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_SYNC_SUBEVENT,
        payload,
        "LE Set Periodic Sync Subevent",
    )

    # Hci.command() restores asynchronous events observed before the matching
    # Command Complete to pending. The completed 0x2084 is the synchronization
    # boundary: those older periodic reports must not supply Request_Event for
    # the following response command.
    _discard_pre_pawr_sync_reports(hci, sync_handle)


def _wait_pawr_periodic_report(
        hci, sync_handle, subevent, marker, timeout=8.0):
    """Wait for a post-0x2084 v2 report for the selected PAwR subevent."""
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META:
            parsed = _periodic_report_data(body)
            if parsed is not None and parsed[0] == sync_handle:
                if body[0] != LE_PERIODIC_ADV_REPORT_V2:
                    continue
                if parsed[2] != subevent:
                    continue
                if marker in parsed[3]:
                    _base._restore(hci, deferred)
                    return parsed
                continue
        deferred.append(packet)
    _base._restore(hci, deferred)
    raise HciError("timed out waiting for selected PAwR periodic advertising marker")


def _set_pawr_response(hci, sync_handle, event_counter, request_subevent,
                       response_subevent):
    marker = PAWR_RESPONSE_MARKER
    payload = struct.pack(
        "<HHBBBB",
        sync_handle,
        event_counter,
        request_subevent,
        response_subevent,
        0,
        len(marker),
    ) + marker
    _command_ok(
        hci,
        OP_LE_SET_PERIODIC_ADV_RESPONSE_DATA,
        payload,
        "LE Set Periodic Advertising Response Data",
    )


def _wait_pawr_response_report(hci, marker, timeout=10.0):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.read_packet(min(0.2, max(0.0, deadline - time.time())))
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META and len(body) >= 5 \
                and body[0] == LE_PERIODIC_ADV_RESPONSE_REPORT:
            if body[1] != ADV_HANDLE_PERIODIC:
                deferred.append(packet)
                continue
            num_responses = body[4]
            at = 5
            for _ in range(num_responses):
                if len(body) < at + 6:
                    break
                data_len = body[at + 5]
                if len(body) < at + 6 + data_len:
                    break
                data = body[at + 6:at + 6 + data_len]
                if marker in data:
                    _base._restore(hci, deferred)
                    return
                at += 6 + data_len
        deferred.append(packet)
    _base._restore(hci, deferred)
    raise HciError("no PAwR response report containing the response marker")


def run_pawr_phase(book, label, advertiser, scanner):
    sync_handle = None
    data_service = None
    pawr_diag_before = None
    try:
        adv_id, adv_type, _ = prepare_controller(advertiser)
        prepare_controller(scanner)
        pawr_diag_before = _pawr_diag_counters(scanner)
        _configure_pawr(advertiser, adv_type)

        # Keep the same start order as ordinary periodic advertising: the
        # associated extended advertiser must be running before the PAwR train.
        _ext_adv_enable(advertiser, ADV_HANDLE_PERIODIC, True)
        _command_ok(
            advertiser,
            OP_LE_SET_PERIODIC_ADV_ENABLE,
            bytes([1, ADV_HANDLE_PERIODIC]),
            "LE Set Periodic Advertising Enable",
        )

        start, count = _wait_pawr_data_request(advertiser)
        subevent = _set_requested_pawr_subevent_data(advertiser, start, count)

        # The advertiser and scanner are separate HCI controllers. Keep one
        # reader per controller so 0x27 maintenance cannot delay scanner 0x25
        # delivery or the following response-critical 0x2083 command.
        data_service = _start_pawr_data_service(advertiser)

        _create_periodic_sync(scanner, adv_id, adv_type)
        sync_handle, _ = _wait_sync_established(
            scanner, timeout=12.0, require_v2=True, keep_scan=True
        )

        # The completed 0x2084 forms the Host/Controller boundary. Reports
        # queued before that completion are discarded by the helper, and only
        # a later v2 report for the selected subevent may drive 0x2083.
        _set_pawr_sync_subevent(scanner, sync_handle, subevent)

        report = _wait_pawr_periodic_report(
            scanner,
            sync_handle,
            subevent,
            PAWR_SUBEVENT_MARKER,
            timeout=12.0,
        )

        # Queue the response before scan shutdown or advertiser-service cleanup
        # can consume any part of the response-slot Host turnaround budget.
        _set_pawr_response(
            scanner,
            sync_handle,
            report[1],
            report[2],
            subevent,
        )

        _finish_pawr_data_service(advertiser, data_service)
        data_service = None
        _set_sync_scan(scanner, False, allow_fail=True)

        pawr_diag_after = _pawr_diag_counters(scanner)
        _verify_pawr_response_completion(
            scanner, pawr_diag_before, pawr_diag_after
        )

        _wait_pawr_response_report(advertiser, PAWR_RESPONSE_MARKER)
        book.passed(
            "PAwR",
            "advertiser and scanner",
            "%s subevent %u + OTA response slot" % (label, subevent),
        )
    except (HciError, HciGone) as err:
        book.failed("PAwR", "advertiser and scanner", str(err))
    finally:
        if data_service is not None:
            try:
                _finish_pawr_data_service(advertiser, data_service)
            except (HciError, HciGone):
                pass
        _set_sync_scan(scanner, False, allow_fail=True)
        if sync_handle is not None:
            _terminate_sync(scanner, sync_handle)
        _stop_periodic(advertiser)

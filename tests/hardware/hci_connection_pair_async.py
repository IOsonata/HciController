import struct
import time
from hci_ble_test import (
    ERROR_NAMES, EVT_DISCONNECTION_COMPLETE, EVT_LE_META, H4_EVENT, HciError)

EVT_REMOTE_VERSION_COMPLETE = 0x0C
LE_CONN_UPDATE_COMPLETE = 0x03
LE_REMOTE_FEATURES_COMPLETE = 0x04
LE_DATA_LENGTH_CHANGE = 0x07
LE_PHY_UPDATE_COMPLETE = 0x0C
LE_PEER_SCA_COMPLETE = 0x1F
LE_TX_POWER_REPORTING = 0x21
LE_SUBRATE_CHANGE = 0x23
LE_ALL_REMOTE_FEATURES_COMPLETE = 0x2B
OP_CONN_UPDATE = 0x2013
OP_REMOTE_FEATURES = 0x2016
OP_SET_DATA_LENGTH = 0x2022
OP_SET_PHY = 0x2032
OP_PEER_SCA = 0x206D
OP_REMOTE_TX_POWER = 0x2077
OP_SUBRATE = 0x207E
OP_ALL_REMOTE_FEATURES = 0x2088
OP_VS_CONN_UPDATE = 0xFD02
OP_VS_REMOTE_TX_POWER = 0xFD0A
STATUS_BUSY = 0x3A

def status_text(status):
    return "0x%02X %s" % (status, ERROR_NAMES.get(status, ""))

def _restore(hci, packets):
    if packets:
        hci.pending = packets + hci.pending

def _wait_for(hci, handle, name, match, timeout=5.0,
              fail_on_disconnect=True):
    deferred = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        packet = hci.pending.pop(0) if hci.pending else hci.read_wire(0.1)
        if packet is None:
            continue
        kind, code, body = packet
        if (fail_on_disconnect and kind == H4_EVENT
                and code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4):
            h = struct.unpack("<H", body[1:3])[0] & 0x0FFF
            if h == handle:
                _restore(hci, deferred)
                raise HciError("%s: link ended with %s"
                               % (name, status_text(body[3])))
        result = match(kind, code, body)
        if result is not None:
            _restore(hci, deferred)
            return result
        deferred.append(packet)
    _restore(hci, deferred)
    raise HciError("timeout waiting for %s on 0x%04X" % (name, handle))

def _wait_classic(hci, handle):
    def match(kind, code, body):
        if kind != H4_EVENT or code != EVT_REMOTE_VERSION_COMPLETE:
            return None
        if len(body) < 3:
            return None
        if (struct.unpack("<H", body[1:3])[0] & 0x0FFF) != handle:
            return None
        return body[0]
    return _wait_for(hci, handle, "Read Remote Version Complete", match)

def _wait_le(hci, handle, subevent, name, handle_offset=2,
             status_offset=1, reason=None):
    def match(kind, code, body):
        if kind != H4_EVENT or code != EVT_LE_META:
            return None
        if len(body) < handle_offset + 2 or body[0] != subevent:
            return None
        h = struct.unpack("<H", body[handle_offset:handle_offset + 2])[0]
        if (h & 0x0FFF) != handle:
            return None
        if reason is not None:
            off, value = reason
            if len(body) <= off or body[off] != value:
                return None
        status = 0 if status_offset is None else body[status_offset]
        return status, body
    return _wait_for(hci, handle, name, match)

def terminal_status(hci, opcode, handle):
    if opcode == 0x041D:
        return _wait_classic(hci, handle)
    if opcode in (OP_CONN_UPDATE, OP_VS_CONN_UPDATE):
        return _wait_le(hci, handle, LE_CONN_UPDATE_COMPLETE,
                        "LE Connection Update Complete")[0]
    if opcode == OP_REMOTE_FEATURES:
        return _wait_le(hci, handle, LE_REMOTE_FEATURES_COMPLETE,
                        "LE Read Remote Features Complete")[0]
    if opcode == OP_SET_DATA_LENGTH:
        _wait_le(hci, handle, LE_DATA_LENGTH_CHANGE, "LE Data Length Change",
                 handle_offset=1, status_offset=None)
        return 0
    if opcode == OP_SET_PHY:
        return _wait_le(hci, handle, LE_PHY_UPDATE_COMPLETE,
                        "LE PHY Update Complete")[0]
    if opcode == OP_REMOTE_TX_POWER:
        return _wait_le(hci, handle, LE_TX_POWER_REPORTING,
                        "LE TX Power Report", reason=(4, 2))[0]
    if opcode == OP_PEER_SCA:
        return _wait_le(hci, handle, LE_PEER_SCA_COMPLETE,
                        "LE Request Peer SCA Complete")[0]
    if opcode == OP_SUBRATE:
        return _wait_le(hci, handle, LE_SUBRATE_CHANGE,
                        "LE Subrate Change")[0]
    if opcode == OP_ALL_REMOTE_FEATURES:
        return _wait_le(hci, handle, LE_ALL_REMOTE_FEATURES_COMPLETE,
                        "LE Read All Remote Features Complete")[0]
    return 0

def verify_fd0a(hci, handle):
    payload = struct.pack("<HB", handle, 1)
    deadline = time.time() + 5.0
    busy = 0
    while time.time() < deadline:
        status, _ = hci.command(OP_REMOTE_TX_POWER, payload, allow_fail=True)
        if status == STATUS_BUSY:
            busy += 1
            time.sleep(0.05)
            continue
        if status != 0:
            raise HciError("post-0xFD0A read returned %s" % status_text(status))
        if busy:
            print("         0xFD0A cleared after %d busy poll(s)" % busy)
        if terminal_status(hci, OP_REMOTE_TX_POWER, handle) != 0:
            raise HciError("post-0xFD0A power report failed")
        return
    raise HciError("0xFD0A remained busy")

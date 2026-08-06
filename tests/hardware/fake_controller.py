#!/usr/bin/env python3
"""
Minimal HCI controller on a pty, used to exercise the test scripts without
hardware. It answers the commands they send and, for the advertise and connect
paths, simulates a peer connecting and driving ATT.

    python3 fake_controller.py                       run hci_test.py
    python3 fake_controller.py --script hci_ble_test.py --args=scan
    python3 fake_controller.py --script hci_ble_test.py \\
        --args="advertise --seconds 6"
    python3 fake_controller.py --serve                print the pty and stay up

Anything not implemented gets Unknown HCI Command, which is what a real
controller must do.
"""

import argparse
import os
import select
import struct
import subprocess
import sys
import threading
import time

H4_COMMAND = 0x01
H4_ACL = 0x02
H4_EVENT = 0x04

EVT_DISCONNECTION_COMPLETE = 0x05
EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_NUM_COMPLETED_PACKETS = 0x13
EVT_LE_META = 0x3E

OP_DISCONNECT = 0x0406
OP_SET_EVENT_MASK = 0x0C01
OP_RESET = 0x0C03
OP_READ_LOCAL_VERSION = 0x1001
OP_READ_BUFFER_SIZE = 0x1005
OP_READ_BD_ADDR = 0x1009
OP_LE_SET_EVENT_MASK = 0x2001
OP_LE_READ_BUFFER_SIZE = 0x2002
OP_LE_READ_LOCAL_FEATURES = 0x2003
OP_LE_SET_RANDOM_ADDRESS = 0x2005
OP_LE_CREATE_CONNECTION = 0x200D
OP_LE_CREATE_CONNECTION_CANCEL = 0x200E
OP_LE_READ_REMOTE_FEATURES = 0x2016
OP_LE_RECEIVER_TEST = 0x201D
OP_LE_TRANSMITTER_TEST = 0x201E
OP_LE_TEST_END = 0x201F
OP_LE_SET_ADV_SET_RANDOM_ADDR = 0x2035
OP_LE_SET_EXT_ADV_PARAMS = 0x2036
OP_LE_SET_EXT_ADV_DATA = 0x2037
OP_LE_SET_EXT_SCAN_RSP_DATA = 0x2038
OP_LE_SET_EXT_ADV_ENABLE = 0x2039
OP_LE_SET_EXT_SCAN_PARAMS = 0x2041
OP_LE_SET_EXT_SCAN_ENABLE = 0x2042
OP_READ_LOCAL_COMMANDS = 0x1002
OP_LE_SET_ADV_PARAMS = 0x2006
OP_LE_READ_ADV_TX_POWER = 0x2007
OP_LE_SET_ADV_DATA = 0x2008
OP_LE_SET_SCAN_RSP_DATA = 0x2009
OP_LE_SET_ADV_ENABLE = 0x200A
OP_LE_SET_SCAN_PARAMS = 0x200B
OP_LE_SET_SCAN_ENABLE = 0x200C
OP_VS_READ_STATIC_ADDRESSES = 0xFC09
OP_VS_READ_COUNTERS = 0xFFF0

# Exactly the dispatch table in src/hci_sdc_nrfxlib.cpp on main. With --subset
# the simulator answers only these, which is what the firmware does today.
SUBSET_OPCODES = (
    OP_SET_EVENT_MASK, OP_RESET, OP_READ_LOCAL_VERSION,
    OP_READ_LOCAL_COMMANDS, 0x1003, OP_READ_BD_ADDR,
    OP_LE_SET_EVENT_MASK, OP_LE_READ_BUFFER_SIZE,
    OP_LE_READ_LOCAL_FEATURES, OP_LE_SET_RANDOM_ADDRESS,
    OP_LE_SET_ADV_PARAMS, OP_LE_READ_ADV_TX_POWER, OP_LE_SET_ADV_DATA,
    OP_LE_SET_SCAN_RSP_DATA, OP_LE_SET_ADV_ENABLE,
    OP_LE_SET_SCAN_PARAMS, OP_LE_SET_SCAN_ENABLE, OP_VS_READ_COUNTERS,
)

CID_ATT = 0x0004
ATT_EXCHANGE_MTU_REQ = 0x02
ATT_EXCHANGE_MTU_RSP = 0x03
ATT_ERROR_RSP = 0x01
ATT_FIND_INFORMATION_REQ = 0x04
ATT_FIND_INFORMATION_RSP = 0x05
ATT_READ_BY_TYPE_REQ = 0x08
ATT_READ_BY_TYPE_RSP = 0x09
ATT_READ_REQ = 0x0A
ATT_READ_RSP = 0x0B
ATT_READ_BY_GROUP_TYPE_REQ = 0x10
ATT_READ_BY_GROUP_TYPE_RSP = 0x11
ATT_WRITE_REQ = 0x12
ATT_WRITE_RSP = 0x13
ATT_HANDLE_VALUE_NTF = 0x1B

# Public address unset, identity carried by the static random address.
BD_ADDR = bytes(6)
STATIC_ADDR = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0xC7])
IDENTITY_ROOT = bytes(range(16))
PEER_ADDR = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0])
CONN_HANDLE = 0x0040

LE_FEATURES = (1 << 0) | (1 << 6) | (1 << 8) | (1 << 11) | (1 << 12)

NO_PARAM_OK = (
    OP_SET_EVENT_MASK, OP_LE_SET_EVENT_MASK, OP_LE_SET_RANDOM_ADDRESS,
    OP_LE_SET_ADV_SET_RANDOM_ADDR, OP_LE_SET_EXT_ADV_DATA,
    OP_LE_SET_EXT_SCAN_RSP_DATA, OP_LE_SET_EXT_SCAN_PARAMS,
    OP_LE_RECEIVER_TEST, OP_LE_TRANSMITTER_TEST,
    OP_LE_SET_ADV_PARAMS, OP_LE_SET_ADV_DATA, OP_LE_SET_SCAN_RSP_DATA,
    OP_LE_SET_SCAN_PARAMS,
)


def command_complete(opcode, status, params=b""):
    body = bytes([0x01]) + struct.pack("<H", opcode) + bytes([status]) + params
    return bytes([H4_EVENT, EVT_COMMAND_COMPLETE, len(body)]) + body


def command_status(opcode, status):
    body = bytes([status, 0x01]) + struct.pack("<H", opcode)
    return bytes([H4_EVENT, EVT_COMMAND_STATUS, len(body)]) + body


def le_meta(body):
    return bytes([H4_EVENT, EVT_LE_META, len(body)]) + body


def enhanced_connection_complete(role):
    body = bytes([0x0A, 0x00])
    body += struct.pack("<H", CONN_HANDLE)
    body += bytes([role, 0x00]) + PEER_ADDR
    body += b"\x00" * 12                      # local and peer resolvable
    body += struct.pack("<HHH", 0x0028, 0, 400)
    body += bytes([0x00])
    return le_meta(body)


def acl_packet(handle, cid, payload):
    l2cap = struct.pack("<HH", len(payload), cid) + payload
    return bytes([H4_ACL]) + struct.pack("<HH", handle, len(l2cap)) + l2cap


def num_completed(handle, count):
    body = bytes([0x01]) + struct.pack("<HH", handle, count)
    return bytes([H4_EVENT, EVT_NUM_COMPLETED_PACKETS, len(body)]) + body


class Controller:
    def __init__(self, fd, verbose=False, subset=False):
        self.fd = fd
        self.verbose = verbose
        self.subset = subset
        self.connected = False
        self.connect_at = None
        self.command_count = 0
        self.acl_taken = 0
        self.test_packets = 0
        self.scanning = False
        self.next_report = None
        self.mtu = 23
        self.services = []
        self.pending_services = []
        self.characteristics = []
        self.cccd_handle = None
        self.subscribed = False
        self.asked_descriptors = False
        self.wrote_rx = False
        self.notifications = 0

    def emit(self, data):
        if self.verbose:
            print("   ctlr ->", data.hex(" "))
        os.write(self.fd, data)

    def on_command(self, opcode, payload):
        if self.subset and opcode not in SUBSET_OPCODES:
            return self.emit(command_complete(opcode, 0x01))

        if opcode in (OP_LE_SET_ADV_ENABLE, OP_LE_SET_SCAN_ENABLE):
            enable = bool(payload and payload[0])
            if opcode == OP_LE_SET_ADV_ENABLE:
                self.connect_at = time.time() + 0.5 if enable else None
            else:
                self.next_report = time.time() + 0.2 if enable else None
            return self.emit(command_complete(opcode, 0x00))

        if opcode == OP_LE_READ_ADV_TX_POWER:
            return self.emit(command_complete(opcode, 0x00, bytes([0x00])))

        if opcode == OP_READ_LOCAL_COMMANDS:
            return self.emit(command_complete(opcode, 0x00, bytes(64)))

        if opcode == OP_RESET:
            self.connected = False
            self.connect_at = None
            return self.emit(command_complete(opcode, 0x00))

        if opcode == OP_READ_LOCAL_VERSION:
            return self.emit(command_complete(
                opcode, 0x00,
                struct.pack("<BHBHH", 17, 4180, 17, 0x0059, 4180)))

        if opcode == OP_READ_BD_ADDR:
            return self.emit(command_complete(opcode, 0x00, BD_ADDR))

        if opcode == OP_VS_READ_STATIC_ADDRESSES:
            return self.emit(command_complete(
                opcode, 0x00, bytes([1]) + STATIC_ADDR + IDENTITY_ROOT))

        if opcode == OP_VS_READ_COUNTERS:
            # Version byte then thirty four fields, little endian, as
            # hci_counters.h lays them out. The command count and the ACL
            # success count are the only ones that move here, which is enough
            # to exercise the script's decoder and its flood arithmetic.
            #
            # The last two are not counters. They are the memory the
            # controller asked for and the memory the build reserved, and they
            # carry the real numbers so the script's headroom line is
            # exercised rather than skipped.
            self.command_count += 1
            counters = [0] * 34
            counters[0] = self.command_count
            counters[16] = self.acl_taken
            counters[32] = 35486
            counters[33] = 35998
            body = bytes([4]) + b"".join(
                struct.pack("<I", v) for v in counters)
            return self.emit(command_complete(opcode, 0x00, body))

        if opcode == OP_READ_BUFFER_SIZE:
            return self.emit(command_complete(opcode, 0x01))

        if opcode == OP_LE_READ_BUFFER_SIZE:
            return self.emit(command_complete(
                opcode, 0x00, struct.pack("<HB", 251, 4)))

        if opcode == OP_LE_READ_LOCAL_FEATURES:
            return self.emit(command_complete(
                opcode, 0x00, LE_FEATURES.to_bytes(8, "little")))

        if opcode == OP_LE_SET_EXT_ADV_PARAMS:
            return self.emit(command_complete(opcode, 0x00, bytes([0x00])))

        if opcode == OP_LE_SET_EXT_ADV_ENABLE:
            enable = payload[0] if payload else 0
            self.connect_at = time.time() + 0.5 if enable else None
            return self.emit(command_complete(opcode, 0x00))

        if opcode == OP_LE_SET_EXT_SCAN_ENABLE:
            self.scanning = bool(payload and payload[0])
            self.next_report = time.time() + 0.2 if self.scanning else None
            return self.emit(command_complete(opcode, 0x00))

        if opcode == OP_LE_CREATE_CONNECTION:
            self.emit(command_status(opcode, 0x00))
            self.connect_at = time.time() + 0.3
            return

        if opcode == OP_LE_CREATE_CONNECTION_CANCEL:
            self.connect_at = None
            return self.emit(command_complete(opcode, 0x00))

        if opcode == OP_LE_READ_REMOTE_FEATURES:
            self.emit(command_status(opcode, 0x00))
            body = bytes([0x04, 0x00]) + struct.pack("<H", CONN_HANDLE)
            body += LE_FEATURES.to_bytes(8, "little")
            return self.emit(le_meta(body))

        if opcode == OP_DISCONNECT:
            self.emit(command_status(opcode, 0x00))
            self.connected = False
            body = bytes([0x00]) + struct.pack("<H", CONN_HANDLE)
            body += bytes([0x16])
            return self.emit(bytes([H4_EVENT, EVT_DISCONNECTION_COMPLETE,
                                    len(body)]) + body)

        if opcode == OP_LE_TEST_END:
            count = self.test_packets
            self.test_packets = 0
            return self.emit(command_complete(
                opcode, 0x00, struct.pack("<H", count)))

        if opcode in NO_PARAM_OK:
            if opcode in (OP_LE_RECEIVER_TEST, OP_LE_TRANSMITTER_TEST):
                self.test_packets = 1234
            return self.emit(command_complete(opcode, 0x00))

        self.emit(command_complete(opcode, 0x01))

    def on_acl(self, handle, cid, payload):
        # Every ACL packet that arrives is one the routing layer handed on, so
        # this stands in for the firmware's AclPutCount.
        self.acl_taken += 1
        # Acknowledge the buffer, which is what returns the host credits.
        self.emit(num_completed(handle, 1))

        if cid != CID_ATT or not payload:
            return

        # Answer an MTU request so the central path can be exercised too.
        if payload[0] == ATT_EXCHANGE_MTU_REQ:
            self.emit(acl_packet(handle, CID_ATT,
                                 struct.pack("<BH", ATT_EXCHANGE_MTU_RSP, 247)))
            return

        # Otherwise act as a central walking the peer's attribute table.
        self.discover(handle, payload)

    def att_send(self, handle, payload):
        self.emit(acl_packet(handle, CID_ATT, payload))

    def discover(self, handle, payload):
        opcode = payload[0]

        if opcode == ATT_EXCHANGE_MTU_RSP:
            self.mtu = struct.unpack("<H", payload[1:3])[0]
            print("   client: agreed MTU %d" % self.mtu)
            self.att_send(handle, struct.pack("<BHHH",
                                              ATT_READ_BY_GROUP_TYPE_REQ,
                                              0x0001, 0xFFFF, 0x2800))
            return

        if opcode == ATT_READ_BY_GROUP_TYPE_RSP:
            size = payload[1]
            body = payload[2:]
            last = 0
            for i in range(0, len(body) - size + 1, size):
                entry = body[i:i + size]
                start, end = struct.unpack("<HH", entry[0:4])
                uuid = entry[4:]
                label = ("0x%04X" % struct.unpack("<H", uuid)[0]
                         if len(uuid) == 2
                         else bytes(reversed(uuid)).hex())
                print("   client: service %s handles 0x%04X to 0x%04X"
                      % (label, start, end))
                self.services.append((start, end))
                last = end
            if last < 0xFFFF:
                self.att_send(handle, struct.pack("<BHHH",
                                                  ATT_READ_BY_GROUP_TYPE_REQ,
                                                  last + 1, 0xFFFF, 0x2800))
            return

        if opcode == ATT_ERROR_RSP:
            failed = payload[1]
            if failed == ATT_READ_BY_GROUP_TYPE_REQ:
                self.pending_services = list(self.services)
                return self.next_characteristic(handle)
            if failed == ATT_READ_BY_TYPE_REQ:
                return self.next_characteristic(handle)
            if failed == ATT_FIND_INFORMATION_REQ:
                return self.subscribe(handle)
            return

        if opcode == ATT_READ_BY_TYPE_RSP:
            size = payload[1]
            body = payload[2:]
            for i in range(0, len(body) - size + 1, size):
                entry = body[i:i + size]
                props = entry[2]
                value_handle = struct.unpack("<H", entry[3:5])[0]
                uuid = entry[5:]
                label = ("0x%04X" % struct.unpack("<H", uuid)[0]
                         if len(uuid) == 2
                         else bytes(reversed(uuid)).hex())
                print("   client: characteristic %s props 0x%02X value 0x%04X"
                      % (label, props, value_handle))
                self.characteristics.append((value_handle, props))
            return self.next_characteristic(handle)

        if opcode == ATT_FIND_INFORMATION_RSP:
            fmt = payload[1]
            size = 2 + (2 if fmt == 0x01 else 16)
            body = payload[2:]
            last = 0
            for i in range(0, len(body) - size + 1, size):
                entry = body[i:i + size]
                att_handle = struct.unpack("<H", entry[0:2])[0]
                uuid = entry[2:]
                last = att_handle
                if len(uuid) == 2 and struct.unpack("<H", uuid)[0] == 0x2902:
                    print("   client: found CCCD at 0x%04X" % att_handle)
                    self.cccd_handle = att_handle

            # A response carries one UUID format only, so keep walking.
            if self.cccd_handle is None and 0 < last < 0x0015:
                self.att_send(handle, struct.pack("<BHH",
                                                  ATT_FIND_INFORMATION_REQ,
                                                  last + 1, 0x0015))
                return
            return self.subscribe(handle)

        if opcode == ATT_WRITE_RSP:
            if not self.wrote_rx:
                self.wrote_rx = True
                self.att_send(handle, struct.pack("<BH", ATT_WRITE_REQ, 0x0012)
                              + b"hello from the client")
            return

        if opcode == ATT_HANDLE_VALUE_NTF:
            self.notifications += 1
            if self.notifications == 1:
                print("   client: notification %s"
                      % payload[3:].decode("utf-8", "replace").strip())
            return

    def next_characteristic(self, handle):
        if self.pending_services:
            start, end = self.pending_services.pop(0)
            self.att_send(handle, struct.pack("<BHHH", ATT_READ_BY_TYPE_REQ,
                                              start, end, 0x2803))
            return
        if not self.asked_descriptors:
            self.asked_descriptors = True
            self.att_send(handle, struct.pack("<BHH",
                                              ATT_FIND_INFORMATION_REQ,
                                              0x0014, 0x0015))

    def subscribe(self, handle):
        if self.cccd_handle and not self.subscribed:
            self.subscribed = True
            self.att_send(handle, struct.pack("<BHH", ATT_WRITE_REQ,
                                              self.cccd_handle, 0x0001))

    def ext_adv_report(self, addr, name, rssi):
        data = bytes([2, 0x01, 0x06])
        data += bytes([len(name) + 1, 0x09]) + name
        body = bytes([0x0D, 0x01])
        body += struct.pack("<H", 0x0013)     # connectable scannable legacy
        body += bytes([0x00]) + addr          # address type, address
        body += bytes([0x01, 0x00, 0x00])     # primary phy, secondary, sid
        body += struct.pack("<b", 0)          # tx power
        body += struct.pack("<b", rssi)
        body += struct.pack("<H", 0)          # periodic interval
        body += bytes([0x00]) + b"\x00" * 6  # direct address type, address
        body += bytes([len(data)]) + data
        return le_meta(body)

    def legacy_adv_report(self, addr, name, rssi):
        data = bytes([2, 0x01, 0x06])
        data += bytes([len(name) + 1, 0x09]) + name
        body = bytes([0x02, 0x01, 0x00, 0x00]) + addr
        body += bytes([len(data)]) + data
        body += struct.pack("<b", rssi)
        return le_meta(body)

    def tick(self):
        if self.next_report and time.time() >= self.next_report:
            self.next_report = time.time() + 0.3
            build = self.legacy_adv_report if self.subset else self.ext_adv_report
            self.emit(build(
                bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06]), b"PeerOne", -42))
            self.emit(build(
                bytes([0x11, 0x12, 0x13, 0x14, 0x15, 0x16]), b"PeerTwo", -70))

        if self.connect_at and time.time() >= self.connect_at:
            self.connect_at = None
            self.connected = True
            self.emit(enhanced_connection_complete(0x01))
            time.sleep(0.05)
            # A phone opens with an MTU exchange.
            self.emit(acl_packet(
                CONN_HANDLE, CID_ATT,
                struct.pack("<BH", ATT_EXCHANGE_MTU_REQ, 517)))


def serve(fd, stop, verbose, subset):
    ctlr = Controller(fd, verbose, subset)
    buf = b""

    while not stop.is_set():
        ctlr.tick()

        # Non blocking, otherwise tick would only run when the host writes.
        ready, _, _ = select.select([fd], [], [], 0.02)
        if ready:
            try:
                chunk = os.read(fd, 512)
            except OSError:
                return
            if chunk:
                buf += chunk

        while buf:
            if buf[0] == H4_COMMAND:
                if len(buf) < 4:
                    break
                length = buf[3]
                if len(buf) < 4 + length:
                    break
                opcode = struct.unpack("<H", buf[1:3])[0]
                payload = buf[4:4 + length]
                buf = buf[4 + length:]
                ctlr.on_command(opcode, payload)
            elif buf[0] == H4_ACL:
                if len(buf) < 5:
                    break
                length = struct.unpack("<H", buf[3:5])[0]
                if len(buf) < 5 + length:
                    break
                handle = struct.unpack("<H", buf[1:3])[0] & 0x0FFF
                l2cap = buf[5:5 + length]
                buf = buf[5 + length:]
                if len(l2cap) >= 4:
                    plen, cid = struct.unpack("<HH", l2cap[0:4])
                    ctlr.on_acl(handle, cid, l2cap[4:4 + plen])
            else:
                buf = buf[1:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--script", default="hci_test.py")
    parser.add_argument("--args", default="")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--subset", action="store_true",
                        help="answer only the opcodes the firmware implements")
    args = parser.parse_args()

    master, slave = os.openpty()
    path = os.ttyname(slave)

    stop = threading.Event()
    worker = threading.Thread(target=serve, args=(master, stop, args.verbose, args.subset),
                              daemon=True)
    worker.start()

    if args.serve:
        print("Controller on %s" % path)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0

    here = os.path.dirname(os.path.abspath(__file__))
    cmd = [sys.executable, os.path.join(here, args.script), "-p", path]
    if args.args:
        cmd += args.args.split()

    result = subprocess.run(cmd)
    stop.set()
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())

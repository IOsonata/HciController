#!/usr/bin/env python3
"""
Over the air validation for the HciController.

Drives the controller as a minimal LE host, so the radio, the link layer and
the ACL data path are all exercised without a Bluetooth stack on the machine.

    python3 hci_ble_test.py scan                  what the radio hears
    python3 hci_ble_test.py advertise             let a phone connect to it
    python3 hci_ble_test.py connect AA:BB:CC:DD:EE:FF
    python3 hci_ble_test.py dtm-tx                radio transmit test
    python3 hci_ble_test.py dtm-rx                radio receive test, gives PER
    python3 hci_ble_test.py counters             what the firmware refused

connect --flood N sends N ACL packets while ignoring flow control, which a host
is not supposed to do. On an nRF52840 the SoftDevice Controller answers 0 to a
packet past the count it advertised and then discards it, credit included, so
the routing layer refuses those itself and hands the credit back. Send more
than the advertised count to see that happen.

advertise is the decisive one. It proves advertising, connection setup,
bidirectional ACL, controller to host flow control and disconnection in a
single run. Connect with nRF Connect and browse the attribute table.
"""

import argparse
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing. Run: pip3 install pyserial")

H4_COMMAND = 0x01
H4_ACL = 0x02
H4_EVENT = 0x04

EVT_DISCONNECTION_COMPLETE = 0x05
EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_NUM_COMPLETED_PACKETS = 0x13
EVT_LE_META = 0x3E

LE_CONNECTION_COMPLETE = 0x01
LE_ADVERTISING_REPORT = 0x02
LE_CONNECTION_UPDATE_COMPLETE = 0x03
LE_READ_REMOTE_FEATURES_COMPLETE = 0x04
LE_LONG_TERM_KEY_REQUEST = 0x05
LE_DATA_LENGTH_CHANGE = 0x07
LE_PHY_UPDATE_COMPLETE = 0x0C
LE_ENHANCED_CONNECTION_COMPLETE = 0x0A
LE_EXTENDED_ADVERTISING_REPORT = 0x0D

OP_DISCONNECT = 0x0406
OP_RESET = 0x0C03
OP_SET_EVENT_MASK = 0x0C01
OP_READ_BD_ADDR = 0x1009
OP_LE_SET_EVENT_MASK = 0x2001
OP_LE_READ_BUFFER_SIZE = 0x2002
OP_LE_SET_RANDOM_ADDRESS = 0x2005
OP_LE_SET_ADV_PARAMS = 0x2006
OP_LE_SET_ADV_DATA = 0x2008
OP_LE_SET_SCAN_RSP_DATA = 0x2009
OP_LE_SET_ADV_ENABLE = 0x200A
OP_LE_SET_SCAN_PARAMS = 0x200B
OP_LE_SET_SCAN_ENABLE = 0x200C
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
OP_LE_EXT_CREATE_CONNECTION = 0x2043

OP_VS_READ_STATIC_ADDRESSES = 0xFC09

# Vendor specific counter readout, HciController's own. See hci_counters.h.
# Order is fixed there; names are appended, never renumbered.
OP_VS_READ_COUNTERS = 0xFFF0
COUNTER_VERSION = 3
COUNTER_NAMES = [
    "commands accepted",
    "unknown opcodes",
    "malformed packets",
    "wrong parameter length",
    "handler errors",
    "event backpressure",
    "ACL refused by controller",
    "ISO refused by controller",
    "controller asked for retry",
    "controller queue errors",
    "unknown output types",
    "bad output lengths",
    "command responses deferred",
    "oversize ACL from host",
    "ISO dropped",
    "credit table overflow",
    "ACL taken by controller",
    "ISO taken by controller",
    "H4 bad packet indicator",
    "H4 oversize packet",
    "H4 delivery retried",
    "transport read errors",
    "transport write errors",
    "transport write deferred",
    "transport tx oversize",
    "host packet retried",
    "host packet rejected",
    "controller get errors",
    "controller packet rejected",
    "controller packet unsendable",
    "host over its ACL credits",
    "link table overflow",
]

CID_ATT = 0x0004
CID_SIGNALING = 0x0005

ATT_ERROR_RSP = 0x01
ATT_EXCHANGE_MTU_REQ = 0x02
ATT_EXCHANGE_MTU_RSP = 0x03
ATT_READ_BY_GROUP_TYPE_REQ = 0x10
ATT_READ_BY_TYPE_REQ = 0x08
ATT_FIND_INFORMATION_REQ = 0x04

ATT_READ_REQ = 0x0A
ATT_READ_RSP = 0x0B
ATT_READ_BLOB_REQ = 0x0C
ATT_READ_BLOB_RSP = 0x0D
ATT_READ_BY_TYPE_RSP = 0x09
ATT_READ_BY_GROUP_TYPE_RSP = 0x11
ATT_FIND_INFORMATION_RSP = 0x05
ATT_WRITE_REQ = 0x12
ATT_WRITE_RSP = 0x13
ATT_WRITE_CMD = 0x52
ATT_HANDLE_VALUE_NTF = 0x1B

ATT_ERR_INVALID_HANDLE = 0x01
ATT_ERR_REQUEST_NOT_SUPPORTED = 0x06
ATT_ERR_INVALID_OFFSET = 0x07
ATT_ERR_ATTRIBUTE_NOT_FOUND = 0x0A
ATT_ERR_UNLIKELY = 0x0E

UUID_PRIMARY_SERVICE = 0x2800
UUID_CHARACTERISTIC = 0x2803
UUID_CCCD = 0x2902
UUID_DEVICE_NAME = 0x2A00
UUID_APPEARANCE = 0x2A01


def uuid128(text):
    """Standard UUID string to the little endian form ATT uses."""
    raw = bytes.fromhex(text.replace("-", ""))
    return bytes(reversed(raw))


# Nordic UART Service, so a scanner app recognises it and offers a terminal.
NUS_SERVICE = uuid128("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
NUS_RX = uuid128("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
NUS_TX = uuid128("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

ERROR_NAMES = {
    0x00: "Success",
    0x01: "Unknown HCI Command",
    0x02: "Unknown Connection Identifier",
    0x08: "Connection Timeout",
    0x0C: "Command Disallowed",
    0x11: "Unsupported Feature or Parameter Value",
    0x12: "Invalid HCI Command Parameters",
    0x13: "Remote User Terminated Connection",
    0x16: "Connection Terminated By Local Host",
    0x3E: "Connection Failed To Be Established",
}


class HciError(Exception):
    pass


def addr_str(raw):
    return ":".join("%02X" % b for b in reversed(raw))


def addr_bytes(text):
    """
    Address text to the little endian order HCI puts on the wire. Takes
    AA:BB:CC:DD:EE:FF, AA-BB-CC-DD-EE-FF or aabbccddeeff, since a scanner app
    or a log will give it in any of those and retyping the colons is a good way
    to mistype the address.
    """
    cleaned = text.replace(":", "").replace("-", "").replace(" ", "")
    if len(cleaned) != 12:
        raise ValueError(
            "%r is not a six octet address. Give it as AA:BB:CC:DD:EE:FF, "
            "AA-BB-CC-DD-EE-FF or aabbccddeeff." % text)
    try:
        raw = bytes.fromhex(cleaned)
    except ValueError:
        raise ValueError("%r has a character that is not a hex digit" % text)
    return bytes(reversed(raw))


class Hci:
    def __init__(self, port, raw=False):
        self.raw = raw
        self.ser = serial.Serial(port, 1000000, timeout=0.05)
        try:
            self.ser.dtr = True
            self.ser.rts = True
        except (OSError, IOError):
            pass
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        self.pending = []
        self.acl_credits = 0
        self.acl_advertised = 0
        self.acl_completed = 0
        self.link_down = None
        self.acl_size = 27
        self.unsupported = []

    def close(self):
        self.ser.close()

    def write_packet(self, data):
        if self.raw:
            print("   tx", data.hex(" "))
        self.ser.write(data)
        self.ser.flush()

    def read_exact(self, count, deadline):
        data = b""
        while len(data) < count:
            if time.time() > deadline:
                raise HciError("timed out after %d of %d bytes"
                               % (len(data), count))
            chunk = self.ser.read(count - len(data))
            if chunk:
                data += chunk
        return data

    def read_packet(self, timeout=1.0):
        """Returns (kind, code, body) or None on timeout."""
        if self.pending:
            return self.pending.pop(0)
        return self.read_wire(timeout)

    def read_wire(self, timeout=1.0):
        """Always reads from the port, never from the deferred queue."""
        deadline = time.time() + timeout
        chunk = self.ser.read(1)
        if not chunk:
            return None

        kind = chunk[0]
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

        raise HciError("bad H:4 packet indicator 0x%02X, stream out of sync"
                       % kind)

    def on_event(self, code, body):
        if code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4:
            # Recorded here rather than in one command's loop, because a link
            # that drops mid test changes what every later measurement means.
            # On disconnection the controller discards whatever it still held
            # for that handle and never counts those packets back, so an
            # unnoticed drop reads as the controller losing data.
            self.link_down = (body[0], struct.unpack("<H", body[1:3])[0],
                              body[3])

        if code == EVT_NUM_COMPLETED_PACKETS and len(body) >= 1:
            count = body[0]
            for i in range(count):
                off = 1 + i * 4
                done = struct.unpack("<H", body[off + 2:off + 4])[0]
                self.acl_credits += done
                # Cumulative, unlike the credit count, which the flood drives
                # negative. This is the controller saying a packet actually
                # went out on air, Vol 4 Part E 7.7.19.
                self.acl_completed += done

    def command(self, opcode, payload=b"", timeout=3.0, allow_fail=False):
        self.write_packet(
            struct.pack("<BHB", H4_COMMAND, opcode, len(payload)) + payload)
        deadline = time.time() + timeout

        # Anything that is not the answer to this command is put back in
        # order once the command completes. Feeding it through the same
        # queue we read from would spin forever.
        deferred = []

        def finish(result):
            self.pending.extend(deferred)
            return result

        while time.time() < deadline:
            packet = self.pending.pop(0) if self.pending else \
                self.read_wire(0.2)
            if packet is None:
                continue

            kind, code, body = packet
            if kind == H4_EVENT and code == EVT_COMMAND_COMPLETE and \
                    struct.unpack("<H", body[1:3])[0] == opcode:
                status = body[3] if len(body) > 3 else 0
                if status != 0 and not allow_fail:
                    self.pending.extend(deferred)
                    raise HciError("opcode 0x%04X returned 0x%02X %s" % (
                        opcode, status, ERROR_NAMES.get(status, "")))
                return finish((status, body[4:]))

            if kind == H4_EVENT and code == EVT_COMMAND_STATUS and \
                    struct.unpack("<H", body[2:4])[0] == opcode:
                status = body[0]
                if status != 0 and not allow_fail:
                    self.pending.extend(deferred)
                    raise HciError("opcode 0x%04X status 0x%02X %s" % (
                        opcode, status, ERROR_NAMES.get(status, "")))
                return finish((status, b""))

            deferred.append(packet)

        self.pending.extend(deferred)
        raise HciError("no event for opcode 0x%04X" % opcode)

    def send_acl(self, handle, cid, payload):
        l2cap = struct.pack("<HH", len(payload), cid) + payload
        # First and only fragment of a higher layer message.
        flags = 0x0000
        header = struct.pack("<HH", handle | (flags << 12), len(l2cap))
        self.write_packet(bytes([H4_ACL]) + header + l2cap)
        self.acl_credits -= 1

    def read_counters(self):
        """
        Vendor specific readout. Returns a list matching COUNTER_NAMES, or None
        if the controller does not carry the command.
        """
        status, data = self.command(OP_VS_READ_COUNTERS, allow_fail=True)
        if status != 0 or not data:
            return None
        version = data[0]
        if version > COUNTER_VERSION:
            print("Counter block version %d, this script reads up to %d. The "
                  "counters it knows are still in the same places."
                  % (version, COUNTER_VERSION))
        values = []
        for i in range(len(COUNTER_NAMES)):
            start = 1 + i * 4
            if start + 4 > len(data):
                break
            values.append(struct.unpack("<I", data[start:start + 4])[0])
        return values

    def setup(self):
        self.command(OP_RESET)
        self.command(OP_SET_EVENT_MASK, bytes.fromhex("ffffffffffffff3f"))
        self.command(OP_LE_SET_EVENT_MASK, bytes.fromhex("ffff000000000000"))
        _, data = self.command(OP_LE_READ_BUFFER_SIZE)
        size, count = struct.unpack("<HB", data[:3])
        if size:
            self.acl_size = size
            self.acl_credits = count
            self.acl_advertised = count
        print("Controller ACL buffers: %d bytes x %d" % (self.acl_size,
                                                         self.acl_credits))

    def identity(self, override=None):
        """
        Public address if one is programmed, else the controller's static
        random address, else one assigned here. A host is allowed to pick a
        static random address itself, so a controller without an identity can
        still be exercised.
        """
        if override:
            return override, 0x01, "assigned on the command line"

        _, data = self.command(OP_READ_BD_ADDR)
        if data[:6] != b"\x00" * 6:
            return data[:6], 0x00, "public, programmed in the controller"

        status, data = self.command(OP_VS_READ_STATIC_ADDRESSES,
                                    allow_fail=True)
        if status == 0 and data and data[0] != 0:
            return data[1:7], 0x01, "static random, from the controller"

        if status == 0x01:
            self.unsupported.append(("0xFC09", "VS Read Static Addresses"))

        # Derive something stable for this session. The top two bits must be
        # set for a static random address.
        seed = struct.pack("<I", int(time.time()) & 0xFFFFFFFF)
        addr = seed + bytes([0x5A, 0xC0 | 0x2A])
        return addr, 0x01, "static random, assigned by this script"


def find_port():
    for info in list_ports.comports():
        if info.vid == 0xCAFE and info.pid == 0x4070:
            if "cu." in info.device or not sys.platform.startswith("darwin"):
                return info.device
    for info in list_ports.comports():
        if info.product and "HCI" in info.product:
            return info.device
    return None


def parse_connection(body):
    """Handles both Connection Complete and its Enhanced form."""
    sub = body[0]
    if sub == LE_CONNECTION_COMPLETE:
        status = body[1]
        handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        role = body[4]
        peer = body[6:12]
        interval, latency, timeout = struct.unpack("<HHH", body[12:18])
    elif sub == LE_ENHANCED_CONNECTION_COMPLETE:
        status = body[1]
        handle = struct.unpack("<H", body[2:4])[0] & 0x0FFF
        role = body[4]
        peer = body[6:12]
        interval, latency, timeout = struct.unpack("<HHH", body[24:30])
    else:
        return None
    return status, handle, role, peer, interval, latency, timeout


def describe_interval(interval, latency, timeout):
    return "interval %.2f ms, latency %d, timeout %d ms" % (
        interval * 1.25, latency, timeout * 10)


class Attribute:
    def __init__(self, handle, uuid, value, group_end=None):
        self.handle = handle
        self.uuid = uuid          # bytes, 2 or 16, little endian
        self.value = value
        self.group_end = group_end


class AttServer:
    """
    A small but real GATT server. Enough for a scanner app to discover the
    Nordic UART Service, read and write the receive characteristic and
    subscribe to notifications, which drives ACL hard in both directions.
    """

    def __init__(self, hci, handle, name):
        self.hci = hci
        self.conn = handle
        self.mtu = 23
        self.requests = 0
        self.responses = 0
        self.notifications = 0
        self.written = b""
        self.cccd = 0
        self.attrs = self.build(name)

    @staticmethod
    def u16(value):
        return struct.pack("<H", value)

    def build(self, name):
        u16 = self.u16
        a = []

        # Generic Access, handles 0x0001 to 0x0005.
        a.append(Attribute(0x0001, u16(UUID_PRIMARY_SERVICE),
                           u16(0x1800), group_end=0x0005))
        a.append(Attribute(0x0002, u16(UUID_CHARACTERISTIC),
                           bytes([0x02]) + u16(0x0003) + u16(UUID_DEVICE_NAME)))
        a.append(Attribute(0x0003, u16(UUID_DEVICE_NAME), name.encode()))
        a.append(Attribute(0x0004, u16(UUID_CHARACTERISTIC),
                           bytes([0x02]) + u16(0x0005) + u16(UUID_APPEARANCE)))
        a.append(Attribute(0x0005, u16(UUID_APPEARANCE), u16(0x0000)))

        # Nordic UART Service, handles 0x0010 to 0x0015.
        a.append(Attribute(0x0010, u16(UUID_PRIMARY_SERVICE),
                           NUS_SERVICE, group_end=0x0015))
        a.append(Attribute(0x0011, u16(UUID_CHARACTERISTIC),
                           bytes([0x0C]) + u16(0x0012) + NUS_RX))
        a.append(Attribute(0x0012, NUS_RX, b""))
        a.append(Attribute(0x0013, u16(UUID_CHARACTERISTIC),
                           bytes([0x10]) + u16(0x0014) + NUS_TX))
        a.append(Attribute(0x0014, NUS_TX, b""))
        a.append(Attribute(0x0015, u16(UUID_CCCD), u16(0x0000)))
        return a

    def find(self, handle):
        for attr in self.attrs:
            if attr.handle == handle:
                return attr
        return None

    def send(self, payload):
        self.hci.send_acl(self.conn, CID_ATT, payload[:self.mtu])
        self.responses += 1

    def error(self, opcode, handle, code):
        self.send(struct.pack("<BBHB", ATT_ERROR_RSP, opcode, handle, code))

    def feed(self, payload, cid):
        if cid != CID_ATT or not payload:
            return

        opcode = payload[0]
        self.requests += 1

        if opcode == ATT_EXCHANGE_MTU_REQ:
            peer = struct.unpack("<H", payload[1:3])[0]
            self.mtu = max(23, min(peer, 247))
            self.send(struct.pack("<BH", ATT_EXCHANGE_MTU_RSP, self.mtu))
            print("   ATT MTU exchange, peer %d, agreed %d" % (peer, self.mtu))
            return

        if opcode == ATT_READ_BY_GROUP_TYPE_REQ:
            return self.read_by_group_type(payload)

        if opcode == ATT_READ_BY_TYPE_REQ:
            return self.read_by_type(payload)

        if opcode == ATT_FIND_INFORMATION_REQ:
            return self.find_information(payload)

        if opcode == ATT_READ_REQ:
            handle = struct.unpack("<H", payload[1:3])[0]
            attr = self.find(handle)
            if attr is None:
                return self.error(opcode, handle, ATT_ERR_INVALID_HANDLE)
            return self.send(bytes([ATT_READ_RSP]) + attr.value[:self.mtu - 1])

        if opcode == ATT_READ_BLOB_REQ:
            handle, offset = struct.unpack("<HH", payload[1:5])
            attr = self.find(handle)
            if attr is None:
                return self.error(opcode, handle, ATT_ERR_INVALID_HANDLE)
            if offset > len(attr.value):
                return self.error(opcode, handle, ATT_ERR_INVALID_OFFSET)
            return self.send(bytes([ATT_READ_BLOB_RSP])
                             + attr.value[offset:offset + self.mtu - 1])

        if opcode in (ATT_WRITE_REQ, ATT_WRITE_CMD):
            handle = struct.unpack("<H", payload[1:3])[0]
            value = payload[3:]
            attr = self.find(handle)
            if attr is None:
                if opcode == ATT_WRITE_REQ:
                    return self.error(opcode, handle, ATT_ERR_INVALID_HANDLE)
                return

            attr.value = value
            if handle == 0x0015:
                self.cccd = struct.unpack("<H", (value + b"\x00\x00")[:2])[0]
                print("   Notifications %s by the peer"
                      % ("enabled" if self.cccd & 0x0001 else "disabled"))
            elif handle == 0x0012:
                self.written += value
                print("   Peer wrote %d bytes: %s" % (
                    len(value), value.decode("utf-8", "replace").strip()))

            if opcode == ATT_WRITE_REQ:
                self.send(bytes([ATT_WRITE_RSP]))
            return

        if opcode % 2 == 0:
            self.error(opcode, 0x0000, ATT_ERR_REQUEST_NOT_SUPPORTED)

    def read_by_group_type(self, payload):
        start, end = struct.unpack("<HH", payload[1:5])
        group = payload[5:]

        if group != self.u16(UUID_PRIMARY_SERVICE):
            return self.error(ATT_READ_BY_GROUP_TYPE_REQ, start,
                              ATT_ERR_ATTRIBUTE_NOT_FOUND)

        entries = []
        size = None
        for attr in self.attrs:
            if attr.group_end is None or not start <= attr.handle <= end:
                continue
            entry = (self.u16(attr.handle) + self.u16(attr.group_end)
                     + attr.value)
            # One response carries entries of a single length only.
            if size is None:
                size = len(entry)
            if len(entry) != size:
                break
            if 2 + len(entries) * size + size > self.mtu:
                break
            entries.append(entry)

        if not entries:
            return self.error(ATT_READ_BY_GROUP_TYPE_REQ, start,
                              ATT_ERR_ATTRIBUTE_NOT_FOUND)

        self.send(bytes([ATT_READ_BY_GROUP_TYPE_RSP, size])
                  + b"".join(entries))

    def read_by_type(self, payload):
        start, end = struct.unpack("<HH", payload[1:5])
        wanted = payload[5:]

        entries = []
        size = None
        for attr in self.attrs:
            if attr.uuid != wanted or not start <= attr.handle <= end:
                continue
            entry = self.u16(attr.handle) + attr.value
            if size is None:
                size = len(entry)
            if len(entry) != size:
                break
            if 2 + len(entries) * size + size > self.mtu:
                break
            entries.append(entry)

        if not entries:
            return self.error(ATT_READ_BY_TYPE_REQ, start,
                              ATT_ERR_ATTRIBUTE_NOT_FOUND)

        self.send(bytes([ATT_READ_BY_TYPE_RSP, size]) + b"".join(entries))

    def find_information(self, payload):
        start, end = struct.unpack("<HH", payload[1:5])

        entries = []
        fmt = None
        for attr in self.attrs:
            if not start <= attr.handle <= end:
                continue
            this_fmt = 0x01 if len(attr.uuid) == 2 else 0x02
            if fmt is None:
                fmt = this_fmt
            if this_fmt != fmt:
                break
            entry = self.u16(attr.handle) + attr.uuid
            if 2 + (len(entries) + 1) * len(entry) > self.mtu:
                break
            entries.append(entry)

        if not entries:
            return self.error(ATT_FIND_INFORMATION_REQ, start,
                              ATT_ERR_ATTRIBUTE_NOT_FOUND)

        self.send(bytes([ATT_FIND_INFORMATION_RSP, fmt]) + b"".join(entries))

    def notify(self, text):
        if not self.cccd & 0x0001:
            return False
        payload = bytes([ATT_HANDLE_VALUE_NTF]) + self.u16(0x0014) + text
        self.hci.send_acl(self.conn, CID_ATT, payload[:self.mtu])
        self.notifications += 1
        return True


def cmd_advertise(hci, args):
    hci.setup()
    override = addr_bytes(args.addr) if args.addr else None
    identity, addr_type, source = hci.identity(override)
    print("Advertising as %s (%s)" % (addr_str(identity), source))

    handle = 0
    name = args.name.encode()
    adv = bytes([2, 0x01, 0x06]) + bytes([len(name) + 1, 0x09]) + name

    # Extended advertising if the controller has it, otherwise legacy.
    params = bytes([handle])
    params += struct.pack("<H", 0x0013)          # connectable scannable legacy
    params += (0x0000A0).to_bytes(3, "little")   # 100 ms
    params += (0x0000A0).to_bytes(3, "little")
    params += bytes([0x07])                      # all three channels
    params += bytes([addr_type])
    params += bytes([0x00])
    params += b"\x00" * 6
    params += bytes([0x00])                      # no filtering
    params += bytes([0x7F])                      # no tx power preference
    params += bytes([0x01])                      # primary PHY 1M
    params += bytes([0x00])
    params += bytes([0x01])                      # secondary PHY 1M
    params += bytes([0x00])
    params += bytes([0x00])

    status, _ = hci.command(OP_LE_SET_EXT_ADV_PARAMS, params, allow_fail=True)
    extended = status == 0

    if extended:
        if addr_type == 0x01:
            hci.command(OP_LE_SET_ADV_SET_RANDOM_ADDR,
                        bytes([handle]) + identity)
        hci.command(OP_LE_SET_EXT_ADV_DATA,
                    bytes([handle, 0x03, 0x01, len(adv)]) + adv)
        hci.command(OP_LE_SET_EXT_SCAN_RSP_DATA,
                    bytes([handle, 0x03, 0x01, 0]))
        hci.command(OP_LE_SET_EXT_ADV_ENABLE,
                    bytes([0x01, 0x01, handle]) + struct.pack("<H", 0)
                    + bytes([0]))
        print("Using extended advertising.")
    else:
        hci.unsupported.append(("0x2036", "LE Set Extended Advertising "
                                          "Parameters"))
        if addr_type == 0x01:
            hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)

        legacy = struct.pack("<HH", 0x00A0, 0x00A0)   # 100 ms
        legacy += bytes([0x00])                       # ADV_IND
        legacy += bytes([addr_type, 0x00])
        legacy += b"\x00" * 6
        legacy += bytes([0x07, 0x00])
        hci.command(OP_LE_SET_ADV_PARAMS, legacy)

        hci.command(OP_LE_SET_ADV_DATA,
                    bytes([len(adv)]) + adv + b"\x00" * (31 - len(adv)))
        hci.command(OP_LE_SET_SCAN_RSP_DATA, bytes(32))
        hci.command(OP_LE_SET_ADV_ENABLE, bytes([0x01]))
        print("Using legacy advertising, the controller has no extended set.")

    print()
    print("On air as %s." % args.name)
    print("In nRF Connect find it in the scanner list and tap CONNECT.")
    print("It exposes the Nordic UART Service. Subscribe to TX to receive,")
    print("write to RX to send.")
    print("Seeing it in the list only proves advertising. The connection is")
    print("what exercises the link layer and the ACL data path.")
    print()

    att = None
    conn_handle = None
    acl_rx = 0
    started = time.time()
    deadline = started + args.seconds
    next_beat = started + 10.0

    next_notify = 0.0
    counter = 0

    while time.time() < deadline:
        if conn_handle is None and time.time() >= next_beat:
            next_beat += 10.0
            print("   still advertising, %d seconds left"
                  % int(deadline - time.time()))

        # Once the peer subscribes, push data so the return path is exercised.
        if att is not None and time.time() >= next_notify \
                and hci.acl_credits > 0:
            counter += 1
            if att.notify(b"tick %d\r\n" % counter):
                next_notify = time.time() + 1.0
            else:
                next_notify = time.time() + 0.5
                counter -= 1

        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet

        if kind == H4_ACL:
            conn = struct.unpack("<H", body[0:2])[0] & 0x0FFF
            length = struct.unpack("<H", body[2:4])[0]
            l2cap = body[4:4 + length]
            if len(l2cap) >= 4:
                plen, cid = struct.unpack("<HH", l2cap[0:4])
                acl_rx += 1
                if att is not None:
                    att.feed(l2cap[4:4 + plen], cid)
            continue

        if kind != H4_EVENT:
            continue

        if code == EVT_LE_META:
            info = parse_connection(body)
            if info is not None:
                status, conn_handle, role, peer, interval, latency, timeout = info
                if status != 0:
                    print("Connection failed, 0x%02X %s"
                          % (status, ERROR_NAMES.get(status, "")))
                    conn_handle = None
                    continue
                print("Connected to %s as %s, handle 0x%04X" % (
                    addr_str(peer),
                    "peripheral" if role == 1 else "central", conn_handle))
                print("   %s" % describe_interval(interval, latency, timeout))
                att = AttServer(hci, conn_handle, args.name)
                hci.command(OP_LE_READ_REMOTE_FEATURES,
                            struct.pack("<H", conn_handle), allow_fail=True)
                continue

            sub = body[0]
            if sub == LE_READ_REMOTE_FEATURES_COMPLETE and body[1] == 0:
                bits = int.from_bytes(body[4:12], "little")
                print("   Peer LE features 0x%016X" % bits)
            elif sub == LE_DATA_LENGTH_CHANGE:
                tx_octets = struct.unpack("<H", body[3:5])[0]
                print("   Data length now %d octets" % tx_octets)
            elif sub == LE_PHY_UPDATE_COMPLETE and body[1] == 0:
                print("   PHY now tx %d rx %d" % (body[4], body[5]))
            elif sub == LE_CONNECTION_UPDATE_COMPLETE and body[1] == 0:
                interval, latency, timeout = struct.unpack("<HHH", body[4:10])
                print("   Updated, %s"
                      % describe_interval(interval, latency, timeout))
            continue

        if code == EVT_DISCONNECTION_COMPLETE:
            reason = body[3]
            print("Disconnected, reason 0x%02X %s"
                  % (reason, ERROR_NAMES.get(reason, "")))
            conn_handle = None
            att = None
            break

    if conn_handle is not None:
        print("Closing the link.")
        status, _ = hci.command(OP_DISCONNECT,
                                struct.pack("<HB", conn_handle, 0x13),
                                allow_fail=True)
        if status == 0x01:
            hci.unsupported.append(("0x0406", "Disconnect"))
            print("   The controller cannot disconnect, the peer must.")
        time.sleep(0.3)
        while hci.read_packet(0.2) is not None:
            pass

    if extended:
        hci.command(OP_LE_SET_EXT_ADV_ENABLE,
                    bytes([0x00, 0x01, handle]) + struct.pack("<H", 0)
                    + bytes([0]), allow_fail=True)
    else:
        hci.command(OP_LE_SET_ADV_ENABLE, bytes([0x00]), allow_fail=True)

    print()
    if att is None and acl_rx == 0:
        print("Advertising ran for %d seconds and nothing connected."
              % args.seconds)
        print()
        print("If the device was visible in a scanner app then advertising,")
        print("the radio and the whole HCI path are working, and the run only")
        print("needs someone to tap connect. Run it again and connect while")
        print("it is waiting.")
        print()
        print("If no scanner saw it at all, that is a different problem and")
        print("worth chasing separately.")
        return 1

    print("ACL received %d, ATT requests %d, responses %d, notifications %d"
          % (acl_rx,
             att.requests if att else 0,
             att.responses if att else 0,
             att.notifications if att else 0))
    if att and att.written:
        print("Peer wrote %d bytes in total." % len(att.written))
    print("Controller ACL credits back to %d" % hci.acl_credits)
    if att and att.responses and hci.acl_credits > 0:
        print("Bidirectional ACL and controller flow control both work.")
        return 0
    print("Data flowed one way only. Check the ACL transmit path.")
    return 1


def parse_ext_report(body):
    # Vol 4 Part E 7.7.65.13.
    if len(body) < 26:
        return None
    data_len = body[25]
    return (addr_str(body[5:11]), struct.unpack("<b", body[15:16])[0],
            body[26:26 + data_len])


def parse_legacy_report(body):
    # Vol 4 Part E 7.7.65.2. Address, then data, then RSSI at the tail.
    if len(body) < 12:
        return None
    data_len = body[10]
    if len(body) < 11 + data_len + 1:
        return None
    return (addr_str(body[4:10]), struct.unpack("<b", body[11 + data_len:
                                                          12 + data_len])[0],
            body[11:11 + data_len])


def local_name(data):
    i = 0
    while i + 1 < len(data):
        field_len = data[i]
        if field_len == 0 or i + 1 + field_len > len(data):
            break
        if data[i + 1] in (0x08, 0x09):
            return data[i + 2:i + 1 + field_len].decode("utf-8", "replace")
        i += field_len + 1
    return ""


def cmd_scan(hci, args):
    hci.setup()

    # Own_Address_Type 0x00 names the public address, and a controller with
    # none rejects the enable with 0x12 rather than the parameters, so the
    # failure lands on the command after the one that is wrong. Resolve the
    # identity the way advertising does and scan with what the board has.
    identity, addr_type, source = hci.identity()
    if addr_type == 0x01:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    print("Scanning as %s (%s)" % (addr_str(identity), source))

    params = bytes([addr_type, 0x00, 0x01])
    params += bytes([0x01 if args.active else 0x00])
    params += struct.pack("<H", 0x0060)
    params += struct.pack("<H", 0x0030)

    status, _ = hci.command(OP_LE_SET_EXT_SCAN_PARAMS, params,
                            allow_fail=True)
    extended = status == 0

    if extended:
        hci.command(OP_LE_SET_EXT_SCAN_ENABLE,
                    bytes([0x01, 0x00]) + struct.pack("<HH", 0, 0))
        print("Using extended scanning.")
    else:
        hci.unsupported.append(("0x2041", "LE Set Extended Scan Parameters"))
        legacy = bytes([0x01 if args.active else 0x00])
        legacy += struct.pack("<HH", 0x0060, 0x0030)
        legacy += bytes([addr_type, 0x00])
        hci.command(OP_LE_SET_SCAN_PARAMS, legacy)
        hci.command(OP_LE_SET_SCAN_ENABLE, bytes([0x01, 0x00]))
        print("Using legacy scanning, the controller has no extended set.")

    print("Scanning for %d seconds." % args.seconds)
    seen = {}
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        if body[0] == LE_EXTENDED_ADVERTISING_REPORT:
            report = parse_ext_report(body)
        elif body[0] == LE_ADVERTISING_REPORT:
            report = parse_legacy_report(body)
        else:
            continue

        if report is None:
            continue

        addr, rssi, data = report
        entry = seen.setdefault(addr, {"count": 0, "rssi": rssi, "name": ""})
        entry["count"] += 1
        entry["rssi"] = max(entry["rssi"], rssi)
        name = local_name(data)
        if name:
            entry["name"] = name

    if extended:
        hci.command(OP_LE_SET_EXT_SCAN_ENABLE,
                    bytes([0x00, 0x00]) + struct.pack("<HH", 0, 0),
                    allow_fail=True)
    else:
        hci.command(OP_LE_SET_SCAN_ENABLE, bytes([0x00, 0x00]),
                    allow_fail=True)

    print()
    print("%d distinct advertisers." % len(seen))
    for addr, info in sorted(seen.items(), key=lambda kv: -kv[1]["count"])[:20]:
        print("   %-18s %4d reports  %4d dBm  %s"
              % (addr, info["count"], info["rssi"], info["name"]))
    return 0 if seen else 1


def cmd_connect(hci, args):
    hci.setup()
    peer = addr_bytes(args.address)

    # Same reason as the scanner: a board with no public address rejects an
    # initiator that names one.
    identity, addr_type, source = hci.identity()
    if addr_type == 0x01:
        hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)
    print("Connecting as %s (%s)" % (addr_str(identity), source))

    params = struct.pack("<HH", 0x0060, 0x0030)   # scan interval, window
    params += bytes([0x00])                        # no filter list
    params += bytes([0x01 if args.random else 0x00])
    params += peer
    params += bytes([addr_type])                   # own address type
    params += struct.pack("<HH", 0x0018, 0x0028)   # conn interval min, max
    params += struct.pack("<HH", 0, 400)           # latency, timeout
    params += struct.pack("<HH", 0, 0)             # ce length

    status, _ = hci.command(OP_LE_CREATE_CONNECTION, params, allow_fail=True)
    if status == 0x01:
        hci.unsupported.append(("0x200D", "LE Create Connection"))
        print("This controller cannot initiate connections.")
        return 1
    if status != 0:
        raise HciError("LE Create Connection returned 0x%02X %s"
                       % (status, ERROR_NAMES.get(status, "")))

    # The canonical form, not what was typed, so a bare hex argument is echoed
    # back the way every other line prints an address.
    print("Connecting to %s, %d second limit."
          % (addr_str(peer), args.seconds))
    deadline = time.time() + args.seconds
    conn_handle = None

    while time.time() < deadline and conn_handle is None:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META:
            info = parse_connection(body)
            if info is None:
                continue
            status, conn_handle, role, addr, interval, latency, timeout = info
            if status != 0:
                print("Failed, 0x%02X %s"
                      % (status, ERROR_NAMES.get(status, "")))
                return 1
            print("Connected as central, handle 0x%04X" % conn_handle)
            print("   %s" % describe_interval(interval, latency, timeout))
            link_timeout = timeout

    if conn_handle is None:
        hci.command(OP_LE_CREATE_CONNECTION_CANCEL, allow_fail=True)
        print("No connection. The peer may not be advertising.")
        return 1

    hci.send_acl(conn_handle, CID_ATT,
                 struct.pack("<BH", ATT_EXCHANGE_MTU_REQ, 247))
    print("Sent ATT Exchange MTU Request.")

    got_response = False
    peer_mtu = 23
    link_timeout = 400
    deadline = time.time() + 5
    while time.time() < deadline and not got_response:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_ACL:
            continue
        length = struct.unpack("<H", body[2:4])[0]
        l2cap = body[4:4 + length]
        if len(l2cap) < 5:
            continue
        _, cid = struct.unpack("<HH", l2cap[0:4])
        if cid == CID_ATT and l2cap[4] == ATT_EXCHANGE_MTU_RSP:
            mtu = struct.unpack("<H", l2cap[5:7])[0]
            print("Peer agreed MTU %d." % mtu)
            peer_mtu = mtu
            got_response = True

    if args.flood:
        flood_acl(hci, conn_handle, args.flood, peer_mtu, link_timeout)

    hci.command(OP_DISCONNECT, struct.pack("<HB", conn_handle, 0x13),
                allow_fail=True)
    time.sleep(0.3)

    if not got_response:
        print("No ATT response. The link came up but data did not flow.")
        return 1

    if hci.link_down is not None:
        _, _, reason = hci.link_down
        print("The link dropped during the run, reason 0x%02X %s, so this run "
              "proves connection setup and ACL but nothing after that point."
              % (reason, ERROR_NAMES.get(reason, "")))
        return 1

    print("Connection and bidirectional ACL both work.")
    return 0


def flood_acl(hci, conn_handle, count, peer_mtu=23, link_timeout=400):
    """
    Send more ACL packets than the controller has buffers, without waiting for
    the Number Of Completed Packets events that hand the credits back. A host
    is not supposed to do this, which is the point: it is the only way to reach
    the path that treats a controller refusal as a retry.

    ATT Write Command to a handle no peer implements, because it draws no
    response, so nothing comes back to muddy the counters and the controller's
    buffers stay full for as long as possible.

    Sized to the MTU the peer agreed. An ATT PDU longer than the agreed MTU is
    a protocol violation and a peer is entitled to drop the link over it, which
    would end the flood early and look exactly like the controller losing the
    packets it had queued.
    """
    before = hci.read_counters()
    if before is None:
        print("No counter readout, so a flood would prove nothing. Skipped.")
        return

    # Opcode and handle are three octets of the ATT PDU.
    value_len = max(1, min(peer_mtu, 247) - 3)
    payload = struct.pack("<BH", ATT_WRITE_CMD, 0xFFFF) + b"\xA5" * value_len
    print()
    print("Sending %d ACL packets of %d octets against %d controller buffers, "
          "ignoring flow control."
          % (count, len(payload) + 4, hci.acl_credits))

    print("Waiting up to %.0f s of quiet, past the %.0f s supervision "
          "timeout, so a peer that stopped acknowledging shows up as a "
          "disconnection rather than as missing packets."
          % (max(6.0, link_timeout * 0.015), link_timeout * 0.01))

    completed_before = hci.acl_completed
    for _ in range(count):
        hci.send_acl(conn_handle, CID_ATT, payload)

    # Drain until the controller has been quiet for a while, rather than for a
    # fixed time, since a slow link needs longer than a fast one to report
    # everything it sent. Capped so a controller that never finishes still ends
    # the run.
    #
    # The clock is read rather than assumed. read_packet's timeout argument
    # does not govern the wait for the first byte, which uses the port timeout,
    # so counting an assumed interval per iteration ends the drain in a
    # fraction of the intended time and reports packets as lost that simply had
    # not been sent yet.
    # The quiet period has to outlast the supervision timeout. A peer that
    # stops acknowledging leaves packets queued in the controller until the
    # link times out, and only then are they discarded and a Disconnection
    # Complete sent. Give up sooner than that and the run ends in the gap: the
    # packets are neither completed nor explained, which reads as the
    # controller losing them when the peer is the one that went away.
    supervision = link_timeout * 0.01
    QUIET_SECONDS = max(6.0, supervision * 1.5)
    DRAIN_LIMIT = max(60.0, supervision * 6.0)
    started = time.time()
    last_progress = started
    while (time.time() - last_progress < QUIET_SECONDS
           and time.time() - started < DRAIN_LIMIT):
        seen = hci.acl_completed
        hci.read_packet(0.2)
        if hci.acl_completed != seen:
            last_progress = time.time()

    drained_for = time.time() - started
    completed = hci.acl_completed - completed_before

    after = hci.read_counters()
    if after is None:
        return

    print("Counters that moved:")
    print_counters(after, before)

    taken = after[16] - before[16]
    refused = after[6] - before[6]
    overrun = after[30] - before[30] if len(after) > 30 else 0
    retries = after[8] - before[8]
    oversize = after[13] - before[13]
    reached = taken + refused + oversize + overrun
    lost = count - reached

    stopped_here = refused + oversize + overrun
    print()
    print("%d of %d packets reached the routing layer. %d went on to "
          "sdc_hci_data_put and %d were refused before it. %d credits came "
          "back in %.1f s."
          % (reached, count, taken, stopped_here, completed, drained_for))

    # The invariant is the host's, not the controller's: every packet it sent
    # spent a buffer, and every one of those has to come back whether the
    # packet was transmitted or refused on the way. Counting credits rather
    # than transmissions is also what makes a refusal here look the same to the
    # host as a success, which is the point of returning them.
    if hci.link_down is not None:
        _, _, reason = hci.link_down
        print("The link dropped during the flood, reason 0x%02X %s."
              % (reason, ERROR_NAMES.get(reason, "")))
        print("On disconnection the controller discards whatever it still held "
              "for that handle and never counts those packets back, so the "
              "shortfall below is explained by the drop and says nothing about "
              "sdc_hci_data_put. Find why the peer hung up before reading it.")
    elif count > completed:
        missing = count - completed
        print("%d buffers never came back within the drain. Vol 4 Part E 4.1.1 "
              "says every packet the controller takes is eventually counted "
              "back, so either they are still queued or they are lost."
              % missing)
        if drained_for >= DRAIN_LIMIT - 1.0:
            print("The drain hit its %g s limit, so the link is most likely "
                  "still working through them. Raise DRAIN_LIMIT before "
                  "reading anything into this." % DRAIN_LIMIT)
        else:
            print("The controller went quiet for %g s with those outstanding, "
                  "which points at loss rather than backlog."
                  % QUIET_SECONDS)
    elif count > 0:
        print("Every buffer the host spent came back, so nothing was lost.")
    if lost > 0:
        # Without this the whole run is unreadable: a block of refusal counters
        # reading zero looks the same whether the controller accepted
        # everything or nothing ever arrived.
        print("%d never got that far, so they were dropped above the routing "
              "layer. Any counter above that moved says where." % lost)
        upstream = [(COUNTER_NAMES[i], after[i] - before[i])
                    for i in range(18, min(30, len(after)))
                    if after[i] != before[i]]
        if upstream:
            for name, delta in upstream:
                print("   %s +%d" % (name, delta))
        else:
            print("   nothing upstream moved either, which points at the host "
                  "or the cable rather than the firmware.")
    elif retries:
        # Never seen on an nRF52840. If it ever fires, sdc_hci.h is incomplete.
        print("sdc_hci_data_put asked for a retry %d times, so the retry path "
              "is live after all and the header's list of return codes is "
              "incomplete." % retries)
    elif refused:
        print("The controller refused %d packets with an error that is not a "
              "retry." % refused)
    elif overrun:
        print("%d were refused here for exceeding the %d buffers the "
              "controller advertised, so they were never handed to SDC, whose "
              "answer to one of those is 0 followed by silently discarding it. "
              "Their credits came back in a Number Of Completed Packets event."
              % (overrun, hci.acl_advertised))
    else:
        print("Nothing was refused, so the host stayed inside the %d buffers "
              "the controller advertised. Send more than that to exercise the "
              "guard." % hci.acl_advertised)
    if oversize:
        print("%d were refused as oversize before reaching the controller."
              % oversize)


def print_counters(values, baseline=None):
    if values is None:
        print("This controller does not carry the counter readout.")
        return
    width = max(len(n) for n in COUNTER_NAMES)
    for i, name in enumerate(COUNTER_NAMES):
        if i >= len(values):
            break
        if baseline is not None:
            delta = values[i] - baseline[i]
            if delta == 0:
                continue
            print("   %-*s %10d  (+%d)" % (width, name, values[i], delta))
        elif values[i]:
            print("   %-*s %10d" % (width, name, values[i]))
    if baseline is None and not any(values):
        print("   all zero")


def cmd_counters(hci, args):
    values = hci.read_counters()
    if values is None:
        hci.unsupported.append(("0xFFF0", "VS Read Counters"))
        print("This controller does not carry the counter readout.")
        return 1
    print("Controller counters, anything not listed is zero:")
    print_counters(values)
    return 0


def cmd_dtm_tx(hci, args):
    hci.command(OP_RESET)
    print("Transmitting on channel %d, payload type %d, %d byte packets."
          % (args.channel, args.payload, args.length))
    hci.command(OP_LE_TRANSMITTER_TEST,
                bytes([args.channel, args.length, args.payload]))
    print("On air for %d seconds. Watch it on a spectrum analyser or a"
          % args.seconds)
    print("second dongle running dtm-rx on the same channel.")
    time.sleep(args.seconds)
    _, data = hci.command(OP_LE_TEST_END)
    print("Test ended, %d packets reported."
          % struct.unpack("<H", data[:2])[0])
    return 0


def cmd_dtm_rx(hci, args):
    hci.command(OP_RESET)
    print("Receiving on channel %d for %d seconds."
          % (args.channel, args.seconds))
    hci.command(OP_LE_RECEIVER_TEST, bytes([args.channel]))
    time.sleep(args.seconds)
    _, data = hci.command(OP_LE_TEST_END)
    received = struct.unpack("<H", data[:2])[0]
    print("Received %d packets." % received)

    if args.expected:
        per = 100.0 * (args.expected - received) / args.expected
        print("Expected %d, packet error rate %.2f percent"
              % (args.expected, per))
        if per > 30.0:
            print("That is high. Check the antenna and the channel.")
            return 1
    elif received == 0:
        print("Nothing received. Either no transmitter is running on this")
        print("channel or the receive path is not working.")
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Over the air validation for the HciController")
    parser.add_argument("-p", "--port")
    parser.add_argument("--raw", action="store_true")
    sub = parser.add_subparsers(dest="mode", required=True)

    p = sub.add_parser("advertise", help="advertise and accept a connection")
    p.add_argument("--name", default="HCI-TEST")
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--addr", help="static random address to advertise with, "
                                  "for a controller with no identity")
    p.set_defaults(func=cmd_advertise)

    p = sub.add_parser("scan", help="list what the radio hears")
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--active", action="store_true",
                   help="send scan requests to collect names")
    p.set_defaults(func=cmd_scan)

    p = sub.add_parser("connect", help="connect to a peer as central")
    p.add_argument("address")
    p.add_argument("--random", action="store_true",
                   help="the peer uses a random address")
    p.add_argument("--seconds", type=int, default=20)
    p.add_argument("--flood", type=int, default=0, metavar="N",
                   help="after connecting, send N ACL packets ignoring flow "
                        "control and report which counters moved")
    p.set_defaults(func=cmd_connect)

    p = sub.add_parser("counters", help="read the controller's own counters")
    p.set_defaults(func=cmd_counters)

    p = sub.add_parser("dtm-tx", help="direct test mode transmitter")
    p.add_argument("--channel", type=int, default=19)
    p.add_argument("--length", type=int, default=37)
    p.add_argument("--payload", type=int, default=0)
    p.add_argument("--seconds", type=int, default=10)
    p.set_defaults(func=cmd_dtm_tx)

    p = sub.add_parser("dtm-rx", help="direct test mode receiver")
    p.add_argument("--channel", type=int, default=19)
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--expected", type=int, default=0,
                   help="packets the transmitter sent, gives a PER figure")
    p.set_defaults(func=cmd_dtm_rx)

    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No HciController found.")
        return 2

    print("Port: %s" % port)
    print()

    hci = Hci(port, raw=args.raw)
    result = 1
    try:
        result = args.func(hci, args)
    except HciError as err:
        print()
        print("HCI error: %s" % err)
    except KeyboardInterrupt:
        result = 130
    finally:
        if hci.unsupported:
            print()
            print("Commands the controller does not implement:")
            for opcode, name in hci.unsupported:
                print("   %s  %s" % (opcode, name))
            print("These are absent from the dispatch table in")
            print("src/hci_sdc_nrfxlib.cpp rather than missing from SDC.")
        hci.close()

    return result


if __name__ == "__main__":
    sys.exit(main())

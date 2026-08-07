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
    python3 hci_ble_test.py counters              what the firmware refused
    python3 hci_ble_test.py probe                 every command, at the radio

probe is the broad one. The firmware dispatches 126 opcodes and this used to
drive 29, so most of what the host tests pinned down had never met a radio.
It sends every command in hci_commands.py, reports what came back, and puts
the controller back where it found it. Commands that change the identity of
the board or leave the radio transmitting need --consent. Commands that need
a link need a handle, and --wait-connect gets one by advertising and letting
a phone connect, which needs no second board.

    python3 hci_ble_test.py probe --consent --wait-connect 30

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
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hci_commands

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
EVT_ENCRYPTION_CHANGE = 0x08
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
COUNTER_VERSION = 4
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

# Indices 32 and 33 are not counters. They are the memory the SoftDevice
# Controller asked for at startup and the memory the build reserved, and they
# are in this block because a sealed dongle has no console to trace them to.
# Kept out of COUNTER_NAMES so nothing treats them as events to be summed or
# differenced.
POOL_FIRST_INDEX = 32
POOL_NAMES = ["SDC pool required", "SDC pool reserved"]

CID_ATT = 0x0004
CID_SIGNALING = 0x0005
CID_SMP = 0x0006

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
    0x41: "Unacceptable Connection Parameters",
    0x42: "Unknown Advertising Identifier",
    0x43: "Limit Reached",
    0x44: "Operation Cancelled By Host",
}


class HciError(Exception):
    pass


class HciGone(Exception):
    """
    The port went away underneath us.

    On a dongle this is not a serial problem. MPSL and the SoftDevice
    Controller reset the chip from their assert handlers by design, see
    HciNrf52840MpslAssert in src/hci_nrf52840.cpp, so a controller fault
    takes the USB device with it and the CDC port disappears. The board then
    re-enumerates and the next run starts clean, which is exactly what makes
    it easy to mistake for a flaky cable.
    """


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
        # Set by on_event when a central asks for the long term key.
        self.ltk_request = None
        # (status, enabled) from Encryption Change, which is the controller
        # saying the pairing above it reached the link layer.
        self.encryption = None
        self.acl_credits = 0
        self.acl_advertised = 0
        self.acl_completed = 0
        self.link_down = None
        self.acl_size = 27
        self.unsupported = []

    def close(self):
        self.ser.close()

    def write_packet(self, data):
        """Raises HciGone when the port has disappeared under us."""
        if self.raw:
            print("   tx", data.hex(" "))
        try:
            self.ser.write(data)
            self.ser.flush()
        except (serial.SerialException, OSError) as err:
            raise HciGone(str(err))

    def read_exact(self, count, deadline):
        """Raises HciGone when the port has disappeared under us."""
        data = b""
        while len(data) < count:
            if time.time() > deadline:
                raise HciError("timed out after %d of %d bytes"
                               % (len(data), count))
            try:
                chunk = self.ser.read(count - len(data))
            except (serial.SerialException, OSError) as err:
                raise HciGone(str(err))
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
        try:
            chunk = self.ser.read(1)
        except (serial.SerialException, OSError) as err:
            # The device node went away. On a dongle that is a controller
            # reset, not a cable problem.
            raise HciGone(str(err))
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

        if code == EVT_LE_META and len(body) >= 3 and \
                body[0] == LE_LONG_TERM_KEY_REQUEST:
            # A central starting encryption asks the peripheral for the key.
            # The link stalls until this is answered and the controller will
            # drop it on the encryption timeout, so a run that ignores the
            # request loses the connection and blames whatever command was in
            # flight when it went. Recorded so the caller can answer it.
            self.ltk_request = struct.unpack("<H", body[1:3])[0]

        if code == EVT_ENCRYPTION_CHANGE and len(body) >= 4:
            self.encryption = (body[0], body[3])

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
        # Read everything present, not just the names this script knows, so
        # the pool figures past the end of COUNTER_NAMES come back too.
        values = []
        available = (len(data) - 1) // 4
        for i in range(available):
            start = 1 + i * 4
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


SMP_PAIRING_REQUEST = 0x01
SMP_PAIRING_RESPONSE = 0x02
SMP_PAIRING_CONFIRM = 0x03
SMP_PAIRING_RANDOM = 0x04
SMP_PAIRING_FAILED = 0x05

SMP_ERR_CONFIRM_FAILED = 0x04

SMP_IO_NO_INPUT_NO_OUTPUT = 0x03
SMP_MAX_KEY_SIZE = 16


def xor16(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


class SmpPeripheral:
    """
    Enough of the Security Manager to let a phone pair, Just Works, legacy.

    Tapping Bond sends a Pairing Request on L2CAP channel 0x0006. A host that
    answers only ATT drops it, pairing never starts, the central never starts
    encryption, and the controller never raises LE Long Term Key Request. So
    the two commands that answer that request could only ever be checked for
    being routed, and link layer encryption was never exercised at all.

    Just Works and legacy on purpose. The temporary key is sixteen zeros,
    which is what Just Works means, and answering with no Secure Connections
    bit keeps the pairing legacy whatever the phone offered. Nothing is
    bonded and no keys are distributed: the point is to reach encryption,
    not to be a security manager anyone should copy.

    AES comes from the controller. LE Encrypt is an AES-128 block on the
    other side of the transport, which is exactly what c1 and s1 need, so
    the crypto here is the firmware's own and this file has none. That also
    means the command gets used for the job it exists for rather than
    checked with a zero key and a zero block.

    The layout of p1, p2 and the s1 input follows Zephyr's smp.c, because
    the specification writes them most significant octet first and every
    field here is on the wire least significant octet first, and getting
    that backwards produces a confirm value that is wrong in a way nothing
    reports except a failed pairing.
    """

    def __init__(self, hci, conn, local_addr, local_type, peer_addr,
                 peer_type):
        self.hci = hci
        self.conn = conn
        # Initiator is the central, which is the peer. Responder is this
        # board, which advertised.
        self.ia = peer_addr
        self.iat = peer_type
        self.ra = local_addr
        self.rat = local_type
        self.tk = bytes(16)
        self.preq = None
        self.pres = None
        self.own_random = None
        self.peer_random = None
        self.peer_confirm = None
        self.stk = None
        self.failed = None
        self.started = False

    def send(self, payload):
        self.hci.send_acl(self.conn, CID_SMP, payload)

    def encrypt(self, key, plaintext):
        """One AES-128 block, done by the controller.

        LE Encrypt takes its key and block most significant octet first, and
        everything here is least significant octet first, so both go in
        reversed and the answer comes back reversed.
        """
        _, data = self.hci.command(0x2017, key[::-1] + plaintext[::-1])
        return data[:16][::-1]

    def random16(self):
        """Sixteen random octets, from the controller's LE Rand."""
        out = b""
        while len(out) < 16:
            _, data = self.hci.command(0x2018)
            out += data[:8]
        return out[:16]

    def c1(self, key, rand):
        """The confirm value. Vol 3 Part H 2.2.3."""
        p1 = bytes([self.iat, self.rat]) + self.preq + self.pres
        first = self.encrypt(key, xor16(rand, p1))
        p2 = self.ra + self.ia + bytes(4)
        return self.encrypt(key, xor16(first, p2))

    def s1(self, key, own_rand, peer_rand):
        """The short term key. Vol 3 Part H 2.2.4.

        The specification writes s1(k, r1, r2) = e(k, r1' || r2') with r1'
        the least significant half of r1 and || putting r1' in the most
        significant half of the result. Every value here is on the wire the
        other way round, so the most significant half is the tail, and the
        octets go in as r2 then r1. Written the way it reads, the answer is
        the right length and wrong, and the only symptom is a pairing that
        never completes. tests/smp_vectors.py runs the worked example from
        Vol 3 Part H D.3 against this, which is how the order was settled.

        For a peripheral the specification calls it s1(TK, Srand, Mrand), so
        r1 is this board's random and r2 is the peer's.
        """
        return self.encrypt(key, peer_rand[:8] + own_rand[:8])

    def feed(self, payload, cid):
        if cid != CID_SMP or not payload:
            return

        code = payload[0]

        if code == SMP_PAIRING_REQUEST and len(payload) >= 7:
            self.started = True
            self.preq = payload[:7]
            # No output and no input, so Just Works. No out of band data,
            # no bonding, no man in the middle protection, no Secure
            # Connections, and nothing distributed either way.
            self.pres = bytes([SMP_PAIRING_RESPONSE,
                               SMP_IO_NO_INPUT_NO_OUTPUT, 0x00, 0x00,
                               SMP_MAX_KEY_SIZE, 0x00, 0x00])
            print("   SMP pairing request, answering Just Works")
            self.send(self.pres)
            return

        if code == SMP_PAIRING_CONFIRM and len(payload) >= 17:
            if self.preq is None:
                return
            self.peer_confirm = payload[1:17]
            self.own_random = self.random16()
            self.send(bytes([SMP_PAIRING_CONFIRM])
                      + self.c1(self.tk, self.own_random))
            return

        if code == SMP_PAIRING_RANDOM and len(payload) >= 17:
            if self.own_random is None:
                return
            self.peer_random = payload[1:17]
            if self.c1(self.tk, self.peer_random) != self.peer_confirm:
                # The peer's confirm does not match the random it just sent,
                # so one of the two is not who it said it was, or this
                # implementation has the octet order wrong.
                self.failed = "the confirm value did not match"
                print("   SMP confirm mismatch, refusing")
                self.send(bytes([SMP_PAIRING_FAILED, SMP_ERR_CONFIRM_FAILED]))
                return
            self.stk = self.s1(self.tk, self.own_random, self.peer_random)
            print("   SMP confirm matched, short term key derived")
            self.send(bytes([SMP_PAIRING_RANDOM]) + self.own_random)
            return

        if code == SMP_PAIRING_FAILED and len(payload) >= 2:
            self.failed = "the peer sent Pairing Failed 0x%02X" % payload[1]
            print("   SMP %s" % self.failed)


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
    width = max(len(n) for n in COUNTER_NAMES + POOL_NAMES)
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

    # "all zero" is about the counters, so the pool figures are excluded from
    # it. They are always set on a controller that started.
    counted = values[:len(COUNTER_NAMES)]
    if baseline is None and not any(counted):
        print("   all zero")

    print_pool(values)


def print_pool(values):
    """
    The two memory figures, printed apart from the counters because they are
    not events. Nothing to add up, and a delta between two readings of them
    would always be zero.
    """
    end = POOL_FIRST_INDEX + len(POOL_NAMES)
    if len(values) < end:
        return

    required, reserved = values[POOL_FIRST_INDEX:end]
    if required == 0 and reserved == 0:
        # A controller whose platform layer never filled them in. Says nothing
        # about how much memory it wanted.
        return

    width = max(len(n) for n in COUNTER_NAMES + POOL_NAMES)
    print()
    for name, value in zip(POOL_NAMES, (required, reserved)):
        print("   %-*s %10d" % (width, name, value))

    if reserved >= required:
        print("   %-*s %10d  headroom" % (width, "", reserved - required))
    else:
        # Cannot happen on a controller that started, since the firmware
        # refuses to enable when the pool is short. Worth saying rather than
        # printing a negative number, in case a future build reports these
        # before the check.
        print("   the controller asked for more than the build reserved, "
              "which should have stopped it starting.")


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


class ProbeContext(object):
    """What a command entry needs resolved before it can be sent."""

    identity = None
    # 0 central, 1 peripheral. A link the probe got by advertising makes it
    # the peripheral, which puts the central only commands out of reach.
    role = None
    # The peer's address as the connection reported it. The pairing confirm
    # is computed over both addresses, so a guess here fails the pairing and
    # nothing says why.
    peer_addr = None
    peer_type = None

    def __init__(self, handle=None, addr_type=0x01):
        self.handle = handle if handle is not None else \
            hci_commands.UNUSED_HANDLE
        # Own_Address_Type. Naming the public address on a board that has
        # none is accepted by the parameter commands and refused by the
        # enables, so the whole run has to agree on what this board is.
        self.addr_type = addr_type


def probe_available(command, args, live_handle, ctx):
    """Whether this entry can be sent, and why not when it cannot."""
    needs = command.needs
    if hci_commands.NEEDS_CONN in needs:
        if args.handle is None:
            return False, "no connection, pass --handle"
        if not live_handle:
            return False, "handle 0x%04X has no connection behind it" \
                % args.handle
    if hci_commands.NEEDS_CENTRAL in needs and ctx.role != 0:
        return False, "central only, and this link has the board as the " \
                      "peripheral"
    if hci_commands.NEEDS_SYNC in needs:
        return False, "needs a periodic sync, so a second radio"
    if hci_commands.NEEDS_CONSENT in needs and not args.consent:
        return False, "changes state or uses the radio, pass --consent"
    if hci_commands.NEEDS_ADV_SET in needs and args.no_adv_set:
        return False, "advertising set not created"
    return True, ""


def probe_wait_for_peer(hci, ctx, args):
    """
    Advertise until something connects, and answer with the handle.

    The twenty eight commands that need a link are the last group with no
    hardware behind them, and a dongle cannot connect to itself. What it can
    do is advertise and let a phone tap connect, which needs no second board
    and no host stack on the machine.

    Connectable undirected, so a scanner app offers to connect. This is the
    one place the probe puts the radio on the air on purpose and leaves it
    there, which is why it is behind a flag.
    """
    handle = 0x00
    name = b"HCI-PROBE"
    adv = bytes([2, 0x01, 0x06, len(name) + 1, 0x09]) + name

    # LE Extended Create Connection was cancelled a moment ago, and a
    # cancelled initiator completes with Unknown Connection Identifier. That
    # event is still queued and is not a failed connection, so it goes before
    # anything here reads a connection complete.
    stale = 0
    while hci.read_packet(0.1) is not None:
        stale += 1
    hci.pending = []

    print()
    if stale:
        print("Cleared %d event(s) left by the initiator that was cancelled."
              % stale)
    print("Advertising as HCI-PROBE for %d seconds. Connect to it with a"
          % args.wait_connect)
    print("phone, nRF Connect or LightBlue, to unlock the commands that need")
    print("a link.")

    hci.command(0x2036,
                bytes([handle]) + struct.pack("<H", 0x0013)
                + b"\xa0\x00\x00" + b"\xf0\x00\x00" + b"\x07"
                + bytes([ctx.addr_type]) + b"\x00" + bytes(6) + b"\x00"
                + struct.pack("<b", 0x7F) + b"\x01\x00\x01\x00\x00")
    if ctx.addr_type == 0x01 and ctx.identity:
        hci.command(0x2035, bytes([handle]) + ctx.identity)
    hci.command(0x2037, bytes([handle, 0x03, 0x01, len(adv)]) + adv)
    hci.command(0x2039,
                bytes([0x01, 0x01, handle]) + struct.pack("<HB", 0, 0))

    deadline = time.time() + args.wait_connect
    conn_handle = None
    while time.time() < deadline:
        packet = hci.read_packet(0.5)
        if packet is None:
            continue
        kind, code, body = packet
        if kind != H4_EVENT or code != EVT_LE_META:
            continue
        info = parse_connection(body)
        if info is None:
            continue
        status, conn_handle, role, peer, _, _, _ = info
        if status != 0:
            print("Connection failed, status 0x%02X" % status)
            conn_handle = None
            continue
        ctx.role = role
        ctx.peer_addr = peer
        ctx.peer_type = body[5]
        print("Connected to %s, handle 0x%04X, this board is the %s."
              % (addr_str(peer), conn_handle,
                 "peripheral" if role == 1 else "central"))
        break

    hci.command(0x2039, bytes([0x00, 0x00]), allow_fail=True)

    if conn_handle is None:
        print("Nothing connected. The commands that need a link stay "
              "skipped.")
    return conn_handle


def probe_service(hci, handlers, seconds):
    """
    Answer whatever the peer asks for, for a while.

    The probe advertises to get a link, and a phone that connects immediately
    tries to discover services. With nothing answering, discovery stalls: the
    packets arrive, hci.command defers them because they are not the event it
    is waiting for, and nothing ever looks at them again. The phone sits
    there until it gives up, and pairing, which is what makes the key request
    rows worth anything, never gets a chance to start.

    So the connection phase serves the same small attribute table the
    advertise command does, between one command and the next.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        queued = hci.pending
        hci.pending = []
        for packet in queued:
            probe_feed(handlers, packet)

        packet = hci.read_packet(0.2)
        if packet is not None:
            probe_feed(handlers, packet)


def probe_feed(handlers, packet):
    """Hand one ACL packet to whichever handler owns its channel."""
    kind, _, body = packet
    if kind != H4_ACL or len(body) < 8:
        return
    length = struct.unpack("<H", body[2:4])[0]
    l2cap = body[4:4 + length]
    if len(l2cap) < 4:
        return
    plen, cid = struct.unpack("<HH", l2cap[0:4])
    payload = l2cap[4:4 + plen]
    for handler in handlers:
        handler.feed(payload, cid)


def probe_answer_ltk(hci, answered):
    """
    Answer a key request that turned up after the waiting was over.

    Tapping Bond a moment late puts the request in the middle of the command
    rows, where nothing is looking for it. Unanswered, the link stalls and
    the controller drops it on the encryption timeout, and the rows after
    that point fail for a reason that has nothing to do with them.

    The negative reply, not the positive one. The key here is zeros, so
    completing encryption with it drops the link on the integrity check,
    which is the outcome this exists to avoid.
    """
    if hci.ltk_request is None or answered:
        return

    handle = hci.ltk_request
    answered.append(handle)
    print("     key request arrived on handle 0x%04X, refusing it so the "
          "link survives" % handle)
    try:
        hci.command(0x201B, struct.pack("<H", handle), timeout=1.0,
                    allow_fail=True)
    except HciError:
        pass


def probe_wait_for_ltk(hci, handlers, smp, args):
    """
    Give a pairing peer time to ask for the long term key, and say so.

    A central that offers to pair starts encryption, and the peripheral's
    controller raises LE Long Term Key Request. Until the host answers it the
    link is stalled, and the controller drops it on the encryption timeout.
    A probe run that ignores the request therefore loses its connection and
    blames whichever command happened to be in flight.

    It also matters for what the run is worth. The two Long Term Key Request
    rows are Command Disallowed with nothing outstanding, which says only
    that the opcode is routed. With a request outstanding they do the thing
    they exist for.
    """
    if hci.ltk_request is not None:
        return

    # The prompt has to come before the wait. Telling someone to tap pair
    # after the window has closed is not a prompt, it is a postmortem.
    print()
    print("Tap Bond or Pair on the phone now. It is answered Just Works,")
    print("with no bonding and nothing kept, and the link ends up encrypted")
    print("with a key derived here. In nRF Connect it is in the menu next to")
    print("Disconnect. Waiting %d seconds." % args.wait_ltk)

    deadline = time.time() + args.wait_ltk
    while time.time() < deadline and hci.ltk_request is None:
        queued = hci.pending
        hci.pending = []
        for packet in queued:
            probe_feed(handlers, packet)
        packet = hci.read_packet(0.2)
        if packet is not None:
            probe_feed(handlers, packet)

    if hci.ltk_request is None:
        if smp.failed:
            print("Pairing did not finish: %s." % smp.failed)
        elif smp.started:
            print("Pairing started but no key request came.")
        else:
            print("Nothing asked. The two reply rows below check that the")
            print("opcode is routed and no more, which is worth having but")
            print("is not the same as answering a request.")
        return

    print("The peer asked for the long term key on handle 0x%04X."
          % hci.ltk_request)

    if smp.stk is None:
        print("No short term key here, so the request is refused. The link")
        print("survives; encryption does not start.")
        hci.command(0x201B, struct.pack("<H", hci.ltk_request),
                    timeout=1.0, allow_fail=True)
        return

    # This is the command doing its job rather than being checked for its
    # shape: a real key, derived from a real pairing, answering a real
    # request.
    status, _ = hci.command(0x201A,
                            struct.pack("<H", hci.ltk_request) + smp.stk,
                            timeout=2.0, allow_fail=True)
    if status != 0:
        print("The key reply was refused, 0x%02X %s."
              % (status, ERROR_NAMES.get(status, "")))
        return

    # Encryption Change is the controller saying the link layer took it.
    deadline = time.time() + 3.0
    while time.time() < deadline and hci.encryption is None:
        packet = hci.read_packet(0.2)
        if packet is not None:
            probe_feed(handlers, packet)

    if hci.encryption is None:
        print("The key was accepted but no Encryption Change arrived.")
    elif hci.encryption[0] == 0 and hci.encryption[1]:
        print("Encryption is on. The link is encrypted with a key this run")
        print("derived, which is the first time that path has been used.")
    else:
        print("Encryption Change said status 0x%02X, enabled %d."
              % hci.encryption)


def cmd_probe(hci, args):
    """
    Send every command in the table and report what came back.

    The dispatch test in tests/unit checks all 126 opcodes reach the intended
    SDC call. What it cannot check is whether the controller accepts the
    command with a real radio behind it, because there is no radio in a host
    build. This is that pass.

    Three answers, and they do not mean the same thing. Accepted means the
    command reached the link layer and it agreed. Refused means a status
    other than success, which is often correct: a controller answers Command
    Disallowed to something out of order and Unsupported Feature to something
    the library does not do on this part. Unknown HCI Command means the
    dispatch table does not carry the opcode at all, and that is the only one
    that is always a defect.

    Where a refusal is the right answer to a well formed parameter block, the
    table says so and the run counts it as a pass. A refusal that the table
    did not predict means one of the two is wrong, and usually it is the
    table: the first two runs of this against a dongle produced eighteen and
    then ten, and all twenty eight were mistakes here.

    The run is in three parts because a controller is in legacy advertising
    mode or extended advertising mode and not both. Whichever set of commands
    is used first, the other is Command Disallowed until reset. So the
    commands that work in either mode go first, then the legacy ones, then a
    reset, then the extended ones.
    """
    # Own_Address_Type has to name an address the board has. Asking for the
    # public address on a controller with none is accepted by the parameter
    # commands and refused by the enables, so the failure lands on the
    # command after the one that is wrong.
    identity, addr_type, source = hci.identity()

    def preamble(label):
        print("%s." % label)
        hci.command(OP_RESET)
        hci.command(OP_SET_EVENT_MASK, bytes.fromhex("ffffffffffffff3f"))
        hci.command(OP_LE_SET_EVENT_MASK, bytes.fromhex("ffff030000000000"))
        if addr_type == 0x01:
            hci.command(OP_LE_SET_RANDOM_ADDRESS, identity)

    preamble("Resetting the controller")
    print("Identity %s (%s)" % (addr_str(identity), source))

    # The command counter only ever rises within one controller lifetime, so
    # a lower reading than the last one means the controller restarted under
    # the run. That has happened here: a direct test mode test torn down too
    # quickly resets this part, and the first symptom was the USB port
    # vanishing. With the settle wait it survives, and what was left was a
    # controller that had quietly forgotten its random address, so
    # LE Set Advertising Enable answered Invalid HCI Command Parameters and
    # read like a bad parameter block.
    #
    # So the counter is sampled at every phase boundary and a fall is
    # reported. A silent restart is the thing worth catching; the rows after
    # it failing is only the symptom.
    restart_watch = {"last": 0}

    def check_alive(where):
        values = hci.read_counters()
        if values is None:
            return
        count = values[0]
        if count < restart_watch["last"]:
            print()
            print("The controller restarted during %s." % where)
            print("Its command counter fell from %d to %d, which it cannot do"
                  % (restart_watch["last"], count))
            print("without the controller having been through a reset. Every")
            print("row after the restart ran against a controller that had")
            print("lost its addresses, its event masks and its advertising")
            print("sets, so their refusals say nothing about their own")
            print("parameter blocks.")
            print()
            restart_watch["last"] = count
            return True
        restart_watch["last"] = count
        return False

    # A handle with nothing behind it turns two dozen rows into two dozen
    # Unknown Connection Identifier lines, which says nothing about any of
    # them. Ask once with a harmless read and skip the group if it is dead.
    live_handle = False
    if args.handle is not None:
        status, _ = hci.command(0x2015, struct.pack("<H", args.handle),
                                allow_fail=True)
        live_handle = status == 0
        if live_handle:
            print("Handle 0x%04X is connected." % args.handle)
        else:
            print("Handle 0x%04X has no connection behind it, 0x%02X %s."
                  % (args.handle, status, ERROR_NAMES.get(status, "")))
            print("The commands that need a link are skipped. Run advertise")
            print("or connect in another shell first, and pass the handle it")
            print("reports.")
    print()

    ctx = ProbeContext(handle=args.handle, addr_type=addr_type)
    ctx.identity = identity
    undo = []
    counts = {"ok": 0, "expected": 0, "silent": 0, "refused": 0,
              "unknown": 0}
    # By opcode rather than a tally, because a command skipped for want of a
    # link is sent later when one arrives, and counting it in both places
    # would say the run covered more than it did.
    skipped = set()

    def report(command, tag, text):
        print("[%s] 0x%04X %-52s %s"
              % (tag, command.opcode, command.name, text))

    def refusal(command, status):
        text = "refused 0x%02X %s" % (status, ERROR_NAMES.get(status, ""))
        if status in command.expect:
            counts["expected"] += 1
            report(command, "ok", text + ", as the row expects")
        else:
            counts["refused"] += 1
            report(command, "  ", text)
            if args.verbose and command.note:
                print("     what the row assumed: %s" % command.note)

    def send(command):
        available, reason = probe_available(command, args, live_handle, ctx)
        if not available:
            skipped.add(command.opcode)
            if args.verbose:
                report(command, "--", reason)
            return
        skipped.discard(command.opcode)

        # A command that answers nothing on success is the one case where no
        # event is the pass. Give it a short window rather than the usual
        # three seconds, since the whole point is that nothing arrives.
        timeout = 0.5 if command.reply == hci_commands.NONE else 3.0

        try:
            status, data = hci.command(command.opcode, command.build(ctx),
                                       timeout=timeout, allow_fail=True)
        except HciGone:
            report(command, "!!", "the port disappeared here")
            raise
        except HciError as err:
            if command.reply == hci_commands.NONE:
                counts["silent"] += 1
                report(command, "ok", "silent, as it must be")
            else:
                counts["unknown"] += 1
                report(command, "!!", str(err))
            status = None

        if status == 0 and command.reply == hci_commands.NONE:
            counts["unknown"] += 1
            report(command, "!!", "answered on success, must be silent")
        elif status == 0:
            counts["ok"] += 1
            report(command, "ok",
                   "%d byte return" % len(data) if data else "no return")
        elif status == 0x01:
            counts["unknown"] += 1
            report(command, "!!", "Unknown HCI Command, not in the table")
        elif status is not None:
            refusal(command, status)

        if command.undo is None:
            return
        if command.undo_now:
            # State that blocks whatever comes next: a direct test mode test
            # still running, an initiator still scanning. Deferring these to
            # the end of the phase loses every command after them.
            #
            # The wait matters. A direct test mode test ended in the same
            # millisecond it was started has reset this controller, so the
            # radio is given time to actually be doing something first.
            if args.settle_ms:
                time.sleep(args.settle_ms / 1000.0)
            try:
                hci.command(command.undo[0], command.undo[1], timeout=1.0,
                            allow_fail=True)
            except HciGone:
                report(command, "!!",
                       "the port disappeared while undoing this")
                raise
            except HciError:
                pass
        else:
            undo.append(command.undo)

    def unwind():
        """
        Put the controller back, most recent first, so an advertising set
        stops advertising before anything tries to remove it. Failures here
        are not interesting: most of these are only needed if the command
        that registered them worked.
        """
        for opcode, payload in reversed(undo):
            try:
                hci.command(opcode, payload, timeout=1.0, allow_fail=True)
            except HciError:
                pass
        del undo[:]

    def run_phase(phase):
        for command in hci_commands.COMMANDS:
            if command.phase != phase:
                continue
            if command.opcode == 0x0C03:
                # Reset is the preamble's job. Sending it as a row would drop
                # the event masks and every set the later rows depend on.
                continue
            send(command)

    check_alive("start up")
    run_phase(hci_commands.PHASE_ANY)

    # Between the two groups rather than at the end, because the direct test
    # mode rows sit in the first group and the legacy advertising rows in the
    # second. Re-asserting the address costs one command and means a restart
    # that happened anyway does not turn the rows after it into a second,
    # invented failure.
    if check_alive("the commands that work in either advertising mode"):
        if addr_type == 0x01:
            hci.command(OP_LE_SET_RANDOM_ADDRESS, identity, allow_fail=True)
            print("Random address set again, so the legacy rows below test")
            print("themselves rather than the restart.")
            print()

    run_phase(hci_commands.PHASE_LEGACY)
    check_alive("the legacy advertising rows")
    unwind()

    print()
    preamble("Resetting, so the extended advertising commands are allowed")
    print()

    check_alive("the reset between the two advertising modes")
    run_phase(hci_commands.PHASE_EXTENDED)
    check_alive("the extended advertising rows")
    unwind()

    for command in hci_commands.TEARDOWN:
        send(command)

    if args.wait_connect and not live_handle:
        handle = probe_wait_for_peer(hci, ctx, args)
        if handle is not None:
            args.handle = handle
            ctx.handle = handle
            live_handle = True
            print()
            print("Running the commands that need a link, on handle 0x%04X."
                  % handle)
            print()
            # A phone that connects starts discovering services at once,
            # and a peripheral that answers nothing leaves it stalled until
            # it gives up. Serve the same small attribute table the
            # advertise command does, so the peer gets its answers and stays
            # long enough to be worth talking to.
            answered = []
            att = AttServer(hci, handle, "HCI-PROBE")
            smp = SmpPeripheral(hci, handle, identity, addr_type,
                                ctx.peer_addr, ctx.peer_type)
            handlers = [att, smp]
            probe_service(hci, handlers, args.discover_secs)
            print("Peer asked %d question(s), answered %d."
                  % (att.requests, att.responses))

            # A phone that offers to pair starts encryption, and the
            # peripheral has to answer the key request or the link dies on
            # the encryption timeout. Waiting for it also turns the two Long
            # Term Key Request rows from a shape check into a real exchange.
            if args.wait_ltk:
                probe_wait_for_ltk(hci, handlers, smp, args)

            # Disconnect ends the link every other row here needs, so it
            # goes last whatever order the table is in.
            linked = [c for c in hci_commands.COMMANDS
                      if hci_commands.NEEDS_CONN in c.needs]

            if hci.ltk_request is not None:
                ctx.handle = hci.ltk_request
                # The negative reply goes first. Answering a real request
                # with the positive reply hands the link layer a key of
                # zeros, encryption completes against a peer that used a
                # different one, and the link drops on the message integrity
                # check, taking every row after it. The negative reply
                # refuses cleanly and leaves the connection up. The positive
                # reply then correctly answers Command Disallowed, since the
                # request has been dealt with.
                order = {0x201B: 0, 0x201A: 1}
                linked.sort(key=lambda c: order.get(c.opcode, -1))

            linked.sort(key=lambda c: c.opcode == 0x0406)
            for command in linked:
                send(command)
                # The peer may still be asking questions. Its packets land in
                # the deferred queue while a command is waiting for its
                # event, and nothing else would ever look at them again.
                queued = hci.pending
                hci.pending = []
                for packet in queued:
                    probe_feed(handlers, packet)
                probe_answer_ltk(hci, answered)
            unwind()

            # Leave nothing connected, whether or not Disconnect was one of
            # the rows that ran.
            hci.command(0x0406, struct.pack("<HB", handle, 0x13),
                        allow_fail=True)

    print()
    print("%d accepted, %d refused as the table expects, %d correctly silent."
          % (counts["ok"], counts["expected"], counts["silent"]))
    print("%d refused otherwise, %d skipped."
          % (counts["refused"], len(skipped)))

    if counts["refused"]:
        print()
        print("A refusal is not by itself a defect. What it does mean is that")
        print("the row did not describe what this controller was going to")
        print("say, so one of the two is wrong. Check the parameter block in")
        print("tests/hardware/hci_commands.py against the vendor header")
        print("before concluding it is the firmware.")

    if counts["unknown"]:
        print()
        print("%d answered Unknown HCI Command. Those opcodes are not in the"
              % counts["unknown"])
        print("dispatch table in src/hci_sdc_nrfxlib.cpp, which the host test")
        print("said they were. That is a real disagreement, not a radio "
              "problem.")
        return 1

    if args.handle is None:
        print()
        print("Connection scoped commands were skipped. Run advertise or")
        print("connect in another shell, then pass --handle with the handle")
        print("it reports.")
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

    p = sub.add_parser("probe", help="send every command the firmware "
                                     "dispatches and report what came back")
    p.add_argument("--handle", type=lambda v: int(v, 0),
                   help="an open connection handle, which unlocks the "
                        "commands that need a link")
    p.add_argument("--consent", action="store_true",
                   help="also send the commands that change the identity of "
                        "the board, put the radio on the air, or end the "
                        "connection being used")
    p.add_argument("--no-adv-set", action="store_true",
                   help="skip the commands that need an advertising set")
    p.add_argument("--verbose", action="store_true",
                   help="say why each skipped command was skipped")
    p.add_argument("--wait-connect", type=int, default=0, metavar="SECONDS",
                   help="after the rest of the run, advertise for this long "
                        "and let a phone connect, then send the commands "
                        "that need a link. A dongle cannot connect to "
                        "itself, and this needs no second board")
    p.add_argument("--discover-secs", type=int, default=5, metavar="SECONDS",
                   help="serve the attribute table for this long after "
                        "connecting, so a phone can finish discovery instead "
                        "of stalling on a peripheral that answers nothing")
    p.add_argument("--wait-ltk", type=int, default=20, metavar="SECONDS",
                   help="once connected, wait this long for a pairing peer "
                        "to ask for the long term key. Long enough to find "
                        "and tap Bond on a phone, which is the point. "
                        "Answering the request is what keeps the link "
                        "alive, and it turns the two reply rows into a real "
                        "exchange. 0 skips the wait")
    p.add_argument("--settle-ms", type=int, default=100, metavar="MS",
                   help="wait this long before undoing a command that puts "
                        "the radio to work, so a direct test mode test is "
                        "running before it is ended. 0 ends it at once, "
                        "which is how the controller reset was found")
    p.set_defaults(func=cmd_probe)

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
    except HciGone as err:
        print()
        print("The port went away: %s" % err)
        print()
        print("On a dongle that is not a cable problem. MPSL and the")
        print("SoftDevice Controller reset the chip from their assert")
        print("handlers, by design, so a controller fault takes the USB")
        print("device with it. The board re-enumerates and the next run")
        print("starts clean, which is what makes this easy to blame on the")
        print("cable. The command named above is where it happened.")
        result = 3
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

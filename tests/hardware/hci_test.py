#!/usr/bin/env python3
"""
End to end test for the HciController H:4 transport over USB CDC or UART.

Sends HCI commands and checks the events that come back. Needs no Bluetooth
stack on the host, so it works the same on macOS, Linux and Windows.

    pip3 install pyserial
    python3 hci_test.py                     auto detect the port
    python3 hci_test.py -p /dev/cu.usbmodem1234
    python3 hci_test.py --advertise         also advertise for 20 seconds
    python3 hci_test.py --scan              also scan for 10 seconds
    python3 hci_test.py --raw               dump every byte in both directions
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

EVT_COMMAND_COMPLETE = 0x0E
EVT_COMMAND_STATUS = 0x0F
EVT_LE_META = 0x3E

OP_RESET = 0x0C03
OP_READ_LOCAL_VERSION = 0x1001
OP_READ_LOCAL_COMMANDS = 0x1002
OP_READ_LOCAL_FEATURES = 0x1003
OP_READ_BUFFER_SIZE = 0x1005
OP_READ_BD_ADDR = 0x1009
OP_SET_EVENT_MASK = 0x0C01
OP_LE_SET_EVENT_MASK = 0x2001
OP_LE_READ_BUFFER_SIZE = 0x2002
OP_LE_READ_LOCAL_FEATURES = 0x2003
OP_LE_READ_MAX_ADV_LEN = 0x203A
OP_LE_SET_EXT_ADV_PARAMS = 0x2036
OP_LE_SET_EXT_ADV_DATA = 0x2037
OP_LE_SET_EXT_ADV_ENABLE = 0x2039
OP_LE_SET_EXT_SCAN_PARAMS = 0x2041
OP_LE_SET_EXT_SCAN_ENABLE = 0x2042

# Zephyr vendor commands, implemented by the Nordic SoftDevice Controller.
OP_VS_READ_STATIC_ADDRESSES = 0xFC09

# Assigned Numbers, Host Controller Interface version. Values through 13 are
# from the published list. 17 was confirmed against this controller.
CORE_VERSIONS = {
    6: "4.0",
    7: "4.1",
    8: "4.2",
    9: "5.0",
    10: "5.1",
    11: "5.2",
    12: "5.3",
    13: "5.4",
    17: "6",
}

# Company id assigned numbers.
COMPANY_IDS = {
    0x0059: "Nordic Semiconductor",
    0x000F: "Broadcom",
    0x001D: "Qualcomm",
}

ERROR_NAMES = {
    0x00: "Success",
    0x01: "Unknown HCI Command",
    0x02: "Unknown Connection Identifier",
    0x0C: "Command Disallowed",
    0x11: "Unsupported Feature or Parameter Value",
    0x12: "Invalid HCI Command Parameters",
}


class HciError(Exception):
    pass


class Hci:
    def __init__(self, port, raw=False):
        self.raw = raw
        self.ser = serial.Serial(port, 1000000, timeout=0.2)
        # The firmware gates the CDC data path on DTR. A plain UART, and a
        # pty when testing without hardware, has no such line.
        try:
            self.ser.dtr = True
            self.ser.rts = True
        except (OSError, IOError):
            pass
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def send_command(self, opcode, payload=b""):
        packet = struct.pack("<BHB", H4_COMMAND, opcode, len(payload)) + payload
        if self.raw:
            print("   tx", packet.hex(" "))
        self.ser.write(packet)
        self.ser.flush()

    def read_exact(self, count, deadline):
        data = b""
        while len(data) < count:
            if time.time() > deadline:
                raise HciError(
                    "timed out after %d of %d bytes" % (len(data), count))
            chunk = self.ser.read(count - len(data))
            if chunk:
                data += chunk
        return data

    def read_packet(self, timeout=2.0):
        deadline = time.time() + timeout
        kind = self.read_exact(1, deadline)[0]

        if kind == H4_EVENT:
            header = self.read_exact(2, deadline)
            body = self.read_exact(header[1], deadline)
            if self.raw:
                print("   rx", bytes([kind]).hex(), header.hex(" "),
                      body.hex(" "))
            return kind, header[0], body

        if kind == H4_ACL:
            header = self.read_exact(4, deadline)
            length = struct.unpack("<H", header[2:4])[0]
            body = self.read_exact(length, deadline)
            if self.raw:
                print("   rx acl", header.hex(" "), body.hex(" "))
            return kind, None, header + body

        raise HciError("bad H:4 packet indicator 0x%02X, stream is out of "
                       "sync" % kind)

    def command(self, opcode, payload=b"", timeout=2.0, allow_fail=False):
        self.send_command(opcode, payload)
        deadline = time.time() + timeout

        while True:
            remaining = max(0.05, deadline - time.time())
            kind, code, body = self.read_packet(remaining)

            if kind != H4_EVENT:
                continue

            if code == EVT_COMMAND_COMPLETE:
                got = struct.unpack("<H", body[1:3])[0]
                if got != opcode:
                    continue
                status = body[3] if len(body) > 3 else 0
                if status != 0 and not allow_fail:
                    raise HciError("opcode 0x%04X returned 0x%02X %s" % (
                        opcode, status,
                        ERROR_NAMES.get(status, "see Vol 1 Part F")))
                return status, body[4:]

            if code == EVT_COMMAND_STATUS:
                got = struct.unpack("<H", body[2:4])[0]
                if got != opcode:
                    continue
                status = body[0]
                if status != 0 and not allow_fail:
                    raise HciError("opcode 0x%04X status 0x%02X %s" % (
                        opcode, status,
                        ERROR_NAMES.get(status, "see Vol 1 Part F")))
                return status, b""


def find_port():
    candidates = []
    for info in list_ports.comports():
        if info.vid == 0xCAFE and info.pid == 0x4070:
            candidates.append(info.device)
        elif info.product and "HCI" in info.product:
            candidates.append(info.device)

    if not candidates:
        return None
    # Prefer the callout device on macOS.
    for name in candidates:
        if "cu." in name:
            return name
    return candidates[0]


def check(label, fn):
    sys.stdout.write("%-34s" % label)
    sys.stdout.flush()
    try:
        detail = fn()
    except HciError as err:
        print("FAIL  %s" % err)
        return False
    except Exception as err:
        print("ERROR %s" % err)
        return False
    print("ok    %s" % (detail or ""))
    return True


def run_basic(hci):
    passed = 0
    total = 0

    def step(label, fn):
        nonlocal passed, total
        total += 1
        if check(label, fn):
            passed += 1

    def do_reset():
        hci.command(OP_RESET)
        return ""

    def do_version():
        _, data = hci.command(OP_READ_LOCAL_VERSION)
        hci_ver, hci_rev, lmp_ver, company, lmp_sub = struct.unpack(
            "<BHBHH", data[:8])
        name = COMPANY_IDS.get(company, "company 0x%04X" % company)

        core = CORE_VERSIONS.get(hci_ver)
        core = "Core %s" % core if core else "unmapped HCI version %d" % hci_ver

        if lmp_ver != hci_ver:
            raise HciError("HCI version %d and LMP version %d disagree"
                           % (hci_ver, lmp_ver))

        return "%s, %s, revision %d, LMP subversion %d" % (
            core, name, hci_rev, lmp_sub)

    def do_bdaddr():
        # The public address. All zeros is normal for an LE only controller
        # with no address programmed, the host is then expected to use a
        # static random address instead.
        _, data = hci.command(OP_READ_BD_ADDR)
        addr = ":".join("%02X" % b for b in reversed(data[:6]))
        if data[:6] == b"\x00" * 6:
            return "public address not programmed, see static address below"
        if data[:6] == b"\xff" * 6:
            raise HciError("public address is all ones, which is invalid")
        return addr

    def do_static_addr():
        status, data = hci.command(OP_VS_READ_STATIC_ADDRESSES,
                                   allow_fail=True)
        if status == 0x01:
            raise HciError("not supported, and the public address is unset, "
                           "so the controller has no usable identity")
        if status != 0:
            raise HciError("status 0x%02X" % status)
        if not data or data[0] == 0:
            raise HciError("controller reports zero static addresses")

        addr = ":".join("%02X" % b for b in reversed(data[1:7]))
        if (data[1 + 5] & 0xC0) != 0xC0:
            raise HciError("%s does not have the top two bits set, so it is "
                           "not a valid static random address" % addr)
        ident = data[7:23]
        root = "identity root present" if any(ident) else "no identity root"
        return "%s random static, %s" % (addr, root)

    def do_buffers():
        # Vol 4 Part E 7.4.5. This is a BR/EDR command. An LE only controller
        # is expected to reject it and the host uses LE Read Buffer Size.
        status, data = hci.command(OP_READ_BUFFER_SIZE, allow_fail=True)
        if status == 0x01:
            return "not supported, correct for an LE only controller"
        if status != 0:
            raise HciError("unexpected status 0x%02X" % status)
        acl_len, _sco_len, acl_num, _sco_num = struct.unpack("<HBHH", data[:7])
        return "ACL %d bytes x %d" % (acl_len, acl_num)

    def do_le_buffers():
        _, data = hci.command(OP_LE_READ_BUFFER_SIZE)
        le_len, le_num = struct.unpack("<HB", data[:3])
        if le_len == 0:
            return "shared with BR/EDR pool"
        return "LE ACL %d bytes x %d" % (le_len, le_num)

    def do_le_features():
        _, data = hci.command(OP_LE_READ_LOCAL_FEATURES)
        bits = int.from_bytes(data[:8], "little")
        names = []
        if bits & (1 << 0):
            names.append("encryption")
        if bits & (1 << 3):
            names.append("ext reject")
        if bits & (1 << 5):
            names.append("ping")
        if bits & (1 << 6):
            names.append("data length ext")
        if bits & (1 << 8):
            names.append("2M PHY")
        if bits & (1 << 11):
            names.append("coded PHY")
        if bits & (1 << 12):
            names.append("ext advertising")
        return ", ".join(names) or "none reported"

    def do_masks():
        hci.command(OP_SET_EVENT_MASK, bytes.fromhex("ffffffffffffff3f"))
        hci.command(OP_LE_SET_EVENT_MASK, bytes.fromhex("ff0f000000000000"))
        return ""

    def do_unknown_is_rejected():
        # Vol 4 Part E 7.4: an unassigned opcode must return 0x01.
        status, _ = hci.command(0x0CFF, allow_fail=True)
        if status != 0x01:
            raise HciError("expected 0x01 Unknown HCI Command, got 0x%02X"
                           % status)
        return "0x01 as required"

    step("HCI Reset", do_reset)
    step("Read Local Version", do_version)
    step("Read BD_ADDR", do_bdaddr)
    step("VS Read Static Addresses", do_static_addr)
    step("Read Buffer Size", do_buffers)
    step("LE Read Buffer Size", do_le_buffers)
    step("LE Read Local Features", do_le_features)
    step("Set event masks", do_masks)
    step("Unknown opcode rejected", do_unknown_is_rejected)

    return passed, total


def run_advertise(hci, seconds):
    print()
    print("Advertising as HCI-TEST for %d seconds." % seconds)
    print("Look for it in nRF Connect or any scanner app.")

    handle = 0
    params = bytes([handle])
    params += struct.pack("<H", 0x0013)          # connectable, legacy, scannable
    params += (0x0000A0).to_bytes(3, "little")   # primary min interval
    params += (0x0000A0).to_bytes(3, "little")   # primary max interval
    params += bytes([0x07])                      # channel map
    params += bytes([0x00])                      # own address type
    params += bytes([0x00])                      # peer address type
    params += b"\x00" * 6                        # peer address
    params += bytes([0x00])                      # filter policy
    params += bytes([0x7F])                      # tx power
    params += bytes([0x01])                      # primary PHY
    params += bytes([0x00])                      # secondary max skip
    params += bytes([0x01])                      # secondary PHY
    params += bytes([0x00])                      # advertising SID
    params += bytes([0x00])                      # scan request notify

    hci.command(OP_LE_SET_EXT_ADV_PARAMS, params)

    name = b"HCI-TEST"
    adv = bytes([2, 0x01, 0x06]) + bytes([len(name) + 1, 0x09]) + name
    data = bytes([handle, 0x03, 0x01, len(adv)]) + adv
    hci.command(OP_LE_SET_EXT_ADV_DATA, data)

    enable = bytes([0x01, 0x01, handle]) + struct.pack("<H", 0) + bytes([0])
    hci.command(OP_LE_SET_EXT_ADV_ENABLE, enable)
    print("Advertising is on.")

    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            kind, code, body = hci.read_packet(0.5)
        except HciError:
            continue
        if kind == H4_EVENT and code == EVT_LE_META:
            print("   LE Meta subevent 0x%02X: %s" % (body[0], body.hex(" ")))

    hci.command(OP_LE_SET_EXT_ADV_ENABLE,
                bytes([0x00, 0x01, handle]) + struct.pack("<H", 0) + bytes([0]))
    print("Advertising stopped.")


def run_scan(hci, seconds):
    print()
    print("Scanning for %d seconds." % seconds)

    params = bytes([0x00, 0x00, 0x01])           # own addr, filter, PHY 1M
    params += bytes([0x00])                      # passive
    params += struct.pack("<H", 0x0060)          # interval
    params += struct.pack("<H", 0x0030)          # window
    hci.command(OP_LE_SET_EXT_SCAN_PARAMS, params)

    enable = bytes([0x01, 0x00]) + struct.pack("<HH", 0, 0)
    hci.command(OP_LE_SET_EXT_SCAN_ENABLE, enable)

    seen = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            kind, code, body = hci.read_packet(0.5)
        except HciError:
            continue
        if kind == H4_EVENT and code == EVT_LE_META and body[0] == 0x0D:
            addr = ":".join("%02X" % b for b in reversed(body[4:10]))
            seen[addr] = seen.get(addr, 0) + 1

    hci.command(OP_LE_SET_EXT_SCAN_ENABLE,
                bytes([0x00, 0x00]) + struct.pack("<HH", 0, 0))
    print("Saw %d distinct advertisers." % len(seen))
    for addr, count in sorted(seen.items(), key=lambda kv: -kv[1])[:10]:
        print("   %s  %d reports" % (addr, count))
    if not seen:
        print("   Nothing heard. The radio is not receiving.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", help="serial port, auto detected if "
                                             "not given")
    parser.add_argument("--advertise", action="store_true",
                        help="advertise so a phone can see the controller")
    parser.add_argument("--scan", action="store_true",
                        help="scan and list what the radio hears")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--raw", action="store_true",
                        help="dump every byte in both directions")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No HciController found. Ports currently present:")
        for info in list_ports.comports():
            print("   %s  %s" % (info.device, info.description))
        return 2

    print("Port: %s" % port)
    print()

    hci = Hci(port, raw=args.raw)
    try:
        passed, total = run_basic(hci)
        print()
        print("%d of %d checks passed." % (passed, total))

        if passed != total:
            return 1

        if args.advertise:
            run_advertise(hci, args.seconds)
        if args.scan:
            run_scan(hci, args.seconds)
    finally:
        hci.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())

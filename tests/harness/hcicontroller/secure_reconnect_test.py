#!/usr/bin/env python3
"""
Pair with a peripheral, drop the link, reconnect, and watch what the secure
reconnect does, one HCI event at a time.

The HciController dongle is the central. Pairing is LE Secure Connections
with Numeric Comparison or Just Works, run by python/hcicontroller/
smp_central.py, so every value the peripheral sees is under this script's
control and every event the dongle's controller raises is printed with a
timestamp from the moment it came off the wire.

Cycle 1 pairs and bonds. The LTK that f5 derived is kept, and optionally
written to a file. Every later cycle connects again, waits for the
peripheral's Security Request (or does not wait, see --start), then issues
LE Enable Encryption with that LTK, Rand 0 and EDIV 0, which is what a bonded
central does after Secure Connections pairing. The result is one of:

  encrypted    Encryption Change, status 0, enabled. An ATT Exchange MTU
               request is then sent over the encrypted link; a response
               proves the peripheral accepted the packets (MIC good).
  dropped      Disconnection Complete with a reason. 0x3D is MIC failure,
               0x06 PIN or Key Missing, 0x08 connection timeout.
  silent       neither inside --enc-timeout.

Examples

  # pair, disconnect, reconnect twice, DUT address known
  secure_reconnect_test.py --address F8:71:FC:89:68:1B --random --cycles 2

  # after resetting the DUT, reconnect with the bond kept in a file
  secure_reconnect_test.py --address ... --bond-file wba.json --reconnect-only

  # negative control: a wrong key must be refused, and shows what a refusal
  # looks like from this side
  secure_reconnect_test.py --address ... --corrupt-ltk

  # do not wait for the peripheral's Security Request
  secure_reconnect_test.py --address ... --start immediate

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
"""

import argparse
import json
import os
import struct
import sys
import time

import _bootstrap  # noqa: F401
from hcicontroller import Hci, SelectionError, TransportSpec, discover
from hcicontroller import hci_ble_test as T
from hcicontroller.hci_events import (
    EVT_DISCONNECTION_COMPLETE, EVT_ENCRYPTION_CHANGE, EVT_LE_META,
    H4_ACL, H4_EVENT, parse_connection)
from hcicontroller import smp_central as S

OP_LE_ENABLE_ENCRYPTION = 0x2019
EVT_ENCRYPTION_KEY_REFRESH = 0x30

REASON = {
    0x05: "Authentication Failure",
    0x06: "PIN or Key Missing",
    0x08: "Connection Timeout",
    0x13: "Remote User Terminated Connection",
    0x14: "Remote Device Terminated, Low Resources",
    0x15: "Remote Device Terminated, Power Off",
    0x16: "Connection Terminated By Local Host",
    0x1A: "Unsupported Remote Feature",
    0x22: "LMP/LL Response Timeout",
    0x23: "LMP/LL Error Transaction Collision",
    0x28: "Instant Passed",
    0x3B: "Unacceptable Connection Parameters",
    0x3D: "Connection Terminated due to MIC Failure",
    0x3E: "Connection Failed to be Established",
}

LE_SUBEVENT = {
    0x01: "LE Connection Complete",
    0x03: "LE Connection Update Complete",
    0x04: "LE Read Remote Features Complete",
    0x05: "LE Long Term Key Request",
    0x07: "LE Data Length Change",
    0x0A: "LE Enhanced Connection Complete",
    0x0C: "LE PHY Update Complete",
    0x12: "LE Channel Selection Algorithm",
    0x29: "LE Enhanced Connection Complete v2",
}


def reason_text(code):
    return "0x%02X %s" % (code, REASON.get(code, ""))


class Clock:
    def __init__(self):
        self.t0 = time.monotonic()

    def ms(self):
        return (time.monotonic() - self.t0) * 1000.0

    def say(self, text):
        print("[%9.1f] %s" % (self.ms(), text), flush=True)


class Link:
    """One controller, with timestamped event logging and L2CAP on top."""

    def __init__(self, hci, clock, verbose):
        self.hci = hci
        self.clock = clock
        self.verbose = verbose
        self.handle = None
        self.reasm = {}
        self.att_seen = 0        # ATT PDUs received on this connection
        self.queue = []          # (cid, payload) completed, in order
        self.events = []         # (time ms, code, body) in order
        self.disconnect = None   # (time ms, status, handle, reason)
        self.encryption = None   # (time ms, status, enabled)
        self.hci_on_event = hci.on_event
        hci.on_event = self._on_event

    def _on_event(self, code, body):
        now = self.clock.ms()
        self.events.append((now, code, body))
        self._log_event(now, code, body)
        if code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4:
            self.disconnect = (now, body[0],
                               struct.unpack("<H", body[1:3])[0], body[3])
        if code == EVT_ENCRYPTION_CHANGE and len(body) >= 4:
            self.encryption = (now, body[0], body[3])
        self.hci_on_event(code, body)

    def _log_event(self, now, code, body):
        if code == 0x13:         # Number Of Completed Packets
            if self.verbose:
                print("[%9.1f] evt   num completed %s" % (now, body.hex()))
            return
        if code == EVT_DISCONNECTION_COMPLETE and len(body) >= 4:
            text = "Disconnection Complete status 0x%02X handle 0x%04X " \
                   "reason %s" % (body[0],
                                  struct.unpack("<H", body[1:3])[0],
                                  reason_text(body[3]))
        elif code == EVT_ENCRYPTION_CHANGE and len(body) >= 4:
            text = "Encryption Change status 0x%02X handle 0x%04X " \
                   "enabled %d" % (body[0],
                                   struct.unpack("<H", body[1:3])[0], body[3])
        elif code == EVT_ENCRYPTION_KEY_REFRESH and len(body) >= 3:
            text = "Encryption Key Refresh status 0x%02X" % body[0]
        elif code == EVT_LE_META and body:
            text = "%s %s" % (LE_SUBEVENT.get(body[0], "LE meta 0x%02X" %
                                              body[0]), body[1:].hex())
        else:
            text = "event 0x%02X %s" % (code, body.hex())
        print("[%9.1f] evt   %s" % (now, text), flush=True)

    # -- ACL --------------------------------------------------------------
    def _take_credit(self, timeout=2.0):
        deadline = time.monotonic() + timeout
        while self.hci.acl_credits <= 0:
            if time.monotonic() > deadline:
                raise T.HciError("no ACL credit from the controller")
            self.poll(0.05)

    def send_l2cap(self, cid, payload):
        l2cap = struct.pack("<HH", len(payload), cid) + payload
        size = max(self.hci.acl_size, 27)
        first = True
        for off in range(0, len(l2cap), size):
            chunk = l2cap[off:off + size]
            self._take_credit()
            pb = 0x0 if first else 0x1
            header = struct.pack("<HH", self.handle | (pb << 12), len(chunk))
            self.hci.write_packet(bytes([H4_ACL]) + header + chunk)
            self.hci.acl_credits -= 1
            first = False

    def _take_acl(self, body):
        word, length = struct.unpack("<HH", body[:4])
        handle, pb = word & 0x0FFF, (word >> 12) & 0x3
        data = body[4:4 + length]
        if pb in (0x0, 0x2):
            self.reasm[handle] = bytearray(data)
        elif handle in self.reasm:
            self.reasm[handle] += data
        else:
            return
        buf = self.reasm[handle]
        if len(buf) < 4:
            return
        need = struct.unpack("<H", buf[:2])[0] + 4
        if len(buf) >= need:
            cid = struct.unpack("<H", buf[2:4])[0]
            self.queue.append((cid, bytes(buf[4:need])))
            del self.reasm[handle]

    def poll(self, timeout=0.1):
        """Read one packet. Returns an (cid, payload) pair if one completed."""
        if self.queue:
            return self._hand_out()
        packet = self.hci.read_packet(timeout)
        if packet is not None:
            kind, _, body = packet
            if kind == H4_ACL:
                self._take_acl(body)
        if self.queue:
            return self._hand_out()
        return None

    def _hand_out(self):
        item = self.queue.pop(0)
        if item[0] == T.CID_ATT:
            self.att_seen += 1
        return item


# ---------------------------------------------------------------------------
def connect(link, peer, peer_type, own_type, timeout, interval=(0x18, 0x28),
            latency=0):
    hci = link.hci
    clock = link.clock
    link.disconnect = None
    link.encryption = None
    link.att_seen = 0
    link.reasm = {}
    link.queue = []

    params = struct.pack("<HH", 0x0060, 0x0030)
    params += bytes([0x00, peer_type]) + peer + bytes([own_type])
    params += struct.pack("<HH", interval[0], interval[1])
    params += struct.pack("<HH", latency, 400)        # supervision 4 s
    params += struct.pack("<HH", 0, 0)
    clock.say("LE Create Connection to %s" % T.addr_str(peer))
    hci.command(T.OP_LE_CREATE_CONNECTION, params)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        packet = hci.read_packet(0.2)
        if packet is None:
            continue
        kind, code, body = packet
        if kind == H4_EVENT and code == EVT_LE_META:
            info = parse_connection(body)
            if info is None:
                continue
            status, handle, role, _, interval, latency, supervision = info
            if status != 0:
                clock.say("connection failed, %s" % reason_text(status))
                return None
            link.handle = handle
            clock.say("connected as central, handle 0x%04X, interval "
                      "%.2f ms, latency %d, supervision %d ms" % (
                          handle, interval * 1.25, latency, supervision * 10))
            return handle
    hci.command(T.OP_LE_CREATE_CONNECTION_CANCEL, allow_fail=True)
    time.sleep(0.3)
    clock.say("no connection inside %.0f s; is the peripheral advertising?" %
              timeout)
    return None


def wait_security_request(link, seconds):
    """Wait for the peripheral's Security Request. Returns the time or None."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if link.disconnect is not None:
            return None
        item = link.poll(0.05)
        if item is None:
            continue
        cid, pdu = item
        if cid == T.CID_SMP and pdu and pdu[0] == S.SMP_SECURITY_REQUEST:
            link.clock.say("rx Security Request, auth 0x%02X" %
                           (pdu[1] if len(pdu) > 1 else 0))
            return link.clock.ms()
        link.clock.say("rx cid 0x%04X %s" % (cid, pdu.hex()))
    return None


def disconnect(link, wait=1.5):
    if link.handle is None:
        return
    if link.disconnect is None:
        link.clock.say("Disconnect (0x13)")
        link.hci.command(T.OP_DISCONNECT, struct.pack("<HB", link.handle,
                                                      0x13), allow_fail=True)
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline and link.disconnect is None:
        link.poll(0.1)
    link.handle = None


def start_encryption(link, ltk, rand=bytes(8), ediv=0):
    link.encryption = None
    link.clock.say("LE Enable Encryption, rand %s ediv 0x%04X ltk %s" % (
        rand.hex(), ediv, ltk.hex()))
    return link.hci.command(OP_LE_ENABLE_ENCRYPTION,
                            struct.pack("<H", link.handle) + rand +
                            struct.pack("<H", ediv) + ltk, allow_fail=True)


def wait_encryption(link, timeout):
    """Returns 'encrypted', 'dropped' or 'silent'."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if link.disconnect is not None:
            return "dropped"
        if link.encryption is not None:
            _, status, enabled = link.encryption
            return "encrypted" if status == 0 and enabled else "refused"
        item = link.poll(0.05)
        if item is not None:
            link.clock.say("rx cid 0x%04X %s" % (item[0], item[1].hex()))
    return "silent"


def att_probe(link, seconds=2.0, mtu=True):
    """
    An ATT request over the (encrypted) link. True when a response comes.
    With mtu False no request is sent: discovery went out before encryption
    and its answers are what is counted.
    """
    if mtu:
        link.send_l2cap(T.CID_ATT,
                        struct.pack("<BH", T.ATT_EXCHANGE_MTU_REQ, 247))
        link.clock.say("tx ATT Exchange MTU Request")
        want = T.ATT_EXCHANGE_MTU_RSP
    else:
        # The requests went out before encryption started; their answers are
        # the evidence, and some may have arrived already.
        if link.att_seen:
            link.clock.say("ATT answered, %d PDUs" % link.att_seen)
            return True
        want = 0x11
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if link.disconnect is not None:
            return False
        item = link.poll(0.05)
        if item is None:
            continue
        cid, pdu = item
        if cid == T.CID_ATT and pdu and pdu[0] in (want, 0x01):
            link.clock.say("rx ATT response 0x%02X %s" % (pdu[0],
                                                          pdu[1:9].hex()))
            return True
        link.clock.say("rx cid 0x%04X %s" % (cid, pdu.hex()))
    return False


def concurrent_traffic(link, args):
    """
    What a phone or nRF Connect does the moment it connects: start ATT
    discovery, ask for 2M and a longer data length, all while encryption is
    being started. The other link layer procedures are in flight when the
    encryption request arrives. None of the responses is waited for.
    """
    handle = link.handle
    if args.phy2m:
        link.clock.say("LE Set PHY, 2M both ways")
        link.hci.command(0x2032, struct.pack("<HBBBH", handle, 0, 0x02, 0x02,
                                             0), allow_fail=True)
    if args.dle:
        link.clock.say("LE Set Data Length 251 / 2120")
        link.hci.command(0x2022, struct.pack("<HHH", handle, 251, 2120),
                         allow_fail=True)
    if args.att_during:
        link.send_l2cap(T.CID_ATT,
                        struct.pack("<BH", T.ATT_EXCHANGE_MTU_REQ, 247))
        link.send_l2cap(T.CID_ATT, struct.pack("<BHHH", 0x10, 1, 0xFFFF,
                                               0x2800))
        link.clock.say("tx ATT Exchange MTU and Read By Group Type, "
                       "not waiting for the answers")


# ---------------------------------------------------------------------------
def pair(link, args, own, own_type, peer, peer_type):
    clock = link.clock
    smp = S.SmpCentral(
        lambda pdu: link.send_l2cap(S.CID_SMP, pdu),
        own, own_type, peer, peer_type,
        io_cap=args.io, auth_req=args.auth, key_size=args.key_size,
        init_keys=args.init_keys, resp_keys=args.resp_keys,
        log=clock.say)

    wait_security_request(link, args.sec_req_wait)
    clock.say("pairing starts")
    smp.start()

    deadline = time.monotonic() + args.pair_timeout
    prompted = False
    while time.monotonic() < deadline and not smp.done:
        if link.disconnect is not None:
            clock.say("link dropped during pairing")
            return None
        item = link.poll(0.05)
        if item is not None:
            cid, pdu = item
            if cid == S.CID_SMP:
                smp.feed(pdu)
            else:
                clock.say("rx cid 0x%04X %s" % (cid, pdu.hex()))
        if smp.state == S.WAIT_USER and not prompted:
            prompted = True
            print()
            print("   Numeric value: %06d" % smp.numeric_value)
            if args.auto_accept:
                print("   Compare it with the DUT, then answer y there. "
                      "Accepted here automatically.")
            else:
                input("   Compare with the DUT and answer y there, then "
                      "press Enter here to accept: ")
            print()
            smp.user_confirm(True)

    if not smp.ok:
        clock.say("pairing did not complete: %s" % (
            smp.failure or "timed out in state %s" % S.STATE_NAMES[smp.state]))
        return None

    clock.say("pairing complete, method %s, LTK %s" % (smp.method,
                                                        smp.ltk.hex()))
    return smp.ltk


def load_bond(path):
    with open(path) as handle:
        raw = json.load(handle)
    return {"ltk": bytes.fromhex(raw["ltk"]),
            "own": bytes.fromhex(raw["own"]), "own_type": raw["own_type"],
            "peer": bytes.fromhex(raw["peer"]), "peer_type": raw["peer_type"]}


def save_bond(path, ltk, own, own_type, peer, peer_type):
    with open(path, "w") as handle:
        json.dump({"ltk": ltk.hex(), "own": own.hex(), "own_type": own_type,
                   "peer": peer.hex(), "peer_type": peer_type}, handle,
                  indent=2)


def params_of(args):
    low = args.interval_min
    high = max(args.interval_max, low)
    return low, high


def run(args):
    try:
        if args.port:
            spec = TransportSpec("serial", args.port,
                                 "serial H:4 %s" % args.port)
        else:
            spec = discover(kind=args.transport, usb_selector=args.usb)
            if spec is None:
                raise SelectionError("no HciController found")
    except SelectionError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    clock = Clock()
    hci = Hci(spec, raw=args.raw)
    link = Link(hci, clock, args.raw)
    results = []
    try:
        hci.setup()

        bond = None
        if args.reconnect_only:
            if not args.bond_file or not os.path.exists(args.bond_file):
                print("FAIL: --reconnect-only needs an existing --bond-file",
                      file=sys.stderr)
                return 2
            bond = load_bond(args.bond_file)
            peer, peer_type = bond["peer"], bond["peer_type"]
            own, own_type = bond["own"], bond["own_type"]
        else:
            peer = T.addr_bytes(args.address)
            peer_type = 0x01 if args.random else 0x00
            own_override = T.addr_bytes(args.own_address) \
                if args.own_address else None
            own, own_type, source = hci.identity(own_override)
            clock.say("central address %s (%s)" % (T.addr_str(own), source))

        # The peripheral finds the bond by the central's address, so a random
        # one has to be the same on every connection of this run.
        if own_type == 0x01:
            hci.command(T.OP_LE_SET_RANDOM_ADDRESS, own)

        ltk = bond["ltk"] if bond else None
        cycle = 0
        if ltk is None:
            handle = connect(link, peer, peer_type, own_type,
                             args.connect_timeout, params_of(args),
                             args.latency)
            if handle is None:
                return 1
            ltk = pair(link, args, own, own_type, peer, peer_type)
            if ltk is None:
                disconnect(link)
                return 1
            if args.bond_file:
                save_bond(args.bond_file, ltk, own, own_type, peer, peer_type)
                clock.say("bond written to %s" % args.bond_file)

            clock.say("LE Enable Encryption on the new link")
            start_encryption(link, ltk)
            outcome = wait_encryption(link, args.enc_timeout)
            ok = outcome == "encrypted" and att_probe(link)
            clock.say("pairing connection: %s%s" % (
                outcome, ", ATT answered" if ok else ""))
            results.append(("pairing connection", outcome, ok))
            if not ok:
                disconnect(link)
                return 1
            time.sleep(args.hold)
            disconnect(link)
            time.sleep(args.gap)

        for cycle in range(1, args.cycles + 1):
            print()
            clock.say("---- reconnect %d of %d ----" % (cycle, args.cycles))
            handle = connect(link, peer, peer_type, own_type,
                             args.connect_timeout, params_of(args),
                             args.latency)
            if handle is None:
                results.append(("reconnect %d" % cycle, "no connection",
                                False))
                break
            t_connect = clock.ms()

            t_request = None
            if args.start == "request":
                t_request = wait_security_request(link, args.sec_req_wait)
                if t_request is None and link.disconnect is None:
                    clock.say("no Security Request in %.1f s, going ahead" %
                              args.sec_req_wait)
            elif args.start_delay:
                time.sleep(args.start_delay / 1000.0)

            if link.disconnect is not None:
                _, _, _, reason = link.disconnect
                clock.say("link dropped before encryption was started: %s" %
                          reason_text(reason))
                results.append(("reconnect %d" % cycle,
                                "dropped %s" % reason_text(reason), False))
                link.handle = None
                time.sleep(args.gap)
                continue

            use = bytearray(ltk)
            if args.corrupt_ltk:
                use[0] ^= 0x01
                clock.say("LTK corrupted on purpose (negative control)")
            concurrent_traffic(link, args)
            t_start = clock.ms()
            start_encryption(link, bytes(use),
                             bytes.fromhex(args.rand)[::-1] if args.rand
                             else bytes(8), args.ediv)
            outcome = wait_encryption(link, args.enc_timeout)
            t_end = clock.ms()

            ok = False
            if outcome == "encrypted":
                ok = att_probe(link, mtu=not args.att_during)
                detail = "encrypted in %.1f ms%s" % (
                    t_end - t_start, ", ATT answered" if ok
                    else ", ATT NOT answered")
            elif outcome == "dropped":
                _, _, _, reason = link.disconnect
                detail = "dropped %.1f ms after LE Enable Encryption, %s" % (
                    t_end - t_start, reason_text(reason))
                if t_request is not None:
                    detail += "; %.1f ms after the Security Request" % (
                        t_end - t_request)
            elif outcome == "refused":
                detail = "Encryption Change status 0x%02X" % link.encryption[1]
            else:
                detail = "nothing in %.1f s" % args.enc_timeout
            clock.say("reconnect %d: %s" % (cycle, detail))
            results.append(("reconnect %d" % cycle, detail, ok))
            _ = t_connect

            if ok:
                time.sleep(args.hold)
            disconnect(link)
            time.sleep(args.gap)
    finally:
        try:
            if link.handle is not None:
                disconnect(link)
        except Exception:
            pass
        hci.close()

    print()
    print("Summary")
    bad = 0
    for name, detail, ok in results:
        print("  %-20s %-5s %s" % (name, "PASS" if ok else "FAIL",
                                   detail if isinstance(detail, str)
                                   else detail))
        bad += 0 if ok else 1
    if args.corrupt_ltk:
        print("  negative control: FAIL rows above are the expected result")
        return 0 if bad == len(results) and results else 1
    return 0 if results and bad == 0 else 1


def main():
    p = argparse.ArgumentParser(
        description="Pair, reconnect and report the secure reconnect, with "
                    "the HciController dongle as the central")
    p.add_argument("--address", help="peripheral address, AA:BB:CC:DD:EE:FF")
    p.add_argument("--random", action="store_true",
                   help="the peripheral address is random, not public")
    p.add_argument("--own-address",
                   help="static random address for the dongle; default is "
                        "the controller's own")
    p.add_argument("--transport", choices=("auto", "serial", "usb"),
                   default="auto")
    p.add_argument("--usb", metavar="SERIAL_OR_VID:PID")
    p.add_argument("--port", help="serial H:4 port instead of discovery")
    p.add_argument("--raw", action="store_true", help="show raw HCI bytes")

    p.add_argument("--cycles", type=int, default=2,
                   help="reconnects after pairing (default 2)")
    p.add_argument("--reconnect-only", action="store_true",
                   help="skip pairing, use the bond in --bond-file")
    p.add_argument("--bond-file", help="write the bond here after pairing, "
                                       "read it with --reconnect-only")
    p.add_argument("--start", choices=("request", "immediate"),
                   default="request",
                   help="start encryption after the peripheral's Security "
                        "Request (default) or as soon as connected")
    p.add_argument("--start-delay", type=int, default=0, metavar="MS",
                   help="with --start immediate, wait this long first")
    p.add_argument("--interval-min", type=int, default=0x18, metavar="UNITS",
                   help="connection interval minimum in 1.25 ms units "
                        "(default 24 = 30 ms; 6 = 7.5 ms)")
    p.add_argument("--interval-max", type=int, default=0x28, metavar="UNITS",
                   help="connection interval maximum in 1.25 ms units "
                        "(default 40 = 50 ms)")
    p.add_argument("--latency", type=int, default=0,
                   help="peripheral latency (default 0)")
    p.add_argument("--phy2m", action="store_true",
                   help="ask for 2M PHY just before starting encryption")
    p.add_argument("--dle", action="store_true",
                   help="ask for data length 251 just before starting "
                        "encryption")
    p.add_argument("--att-during", action="store_true",
                   help="send ATT discovery requests just before starting "
                        "encryption, without waiting for the answers")
    p.add_argument("--rand", metavar="HEX16",
                   help="Rand sent with LE Enable Encryption on reconnect, 16 "
                        "hex digits as written, default 0 (Secure Connections)")
    p.add_argument("--ediv", type=lambda v: int(v, 0), default=0,
                   help="EDIV sent with LE Enable Encryption on reconnect, "
                        "default 0")
    p.add_argument("--corrupt-ltk", action="store_true",
                   help="negative control: flip one bit of the reconnect key")

    p.add_argument("--io", type=int, default=S.IO_DISPLAY_YES_NO,
                   help="IO capability: 1 display yes/no (default), "
                        "3 no input no output")
    p.add_argument("--auth", type=lambda v: int(v, 0), default=0x0D,
                   help="AuthReq, default 0x0D: bonding, MITM, SC")
    p.add_argument("--key-size", type=int, default=16)
    p.add_argument("--init-keys", type=lambda v: int(v, 0), default=0)
    p.add_argument("--resp-keys", type=lambda v: int(v, 0), default=0)
    p.add_argument("--auto-accept", action="store_true",
                   help="do not wait for Enter at the numeric comparison")

    p.add_argument("--connect-timeout", type=float, default=15.0)
    p.add_argument("--sec-req-wait", type=float, default=2.0,
                   help="how long to wait for a Security Request")
    p.add_argument("--pair-timeout", type=float, default=60.0)
    p.add_argument("--enc-timeout", type=float, default=5.0)
    p.add_argument("--hold", type=float, default=1.0,
                   help="seconds to stay connected after a good result")
    p.add_argument("--gap", type=float, default=1.5,
                   help="seconds between a disconnect and the next connect")
    args = p.parse_args()

    if not args.reconnect_only and not args.address:
        p.error("--address is required unless --reconnect-only")
    if args.usb and args.port:
        p.error("--usb and --port are mutually exclusive")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

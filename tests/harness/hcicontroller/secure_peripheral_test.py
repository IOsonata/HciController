#!/usr/bin/env python3
"""
The dongle as a secure peripheral, to watch what a central really does.

secure_reconnect_test.py makes the dongle the central and proves a peripheral
under test. This is the reverse: the dongle advertises as a bondable
peripheral with LE Secure Connections, and nRF Connect, a phone or any other
central connects to it. Everything the central sends is logged with a
timestamp from the moment it came off the wire:

  * its Pairing Request in full: IO capability, AuthReq bits, key size, and
    which keys it asks to be distributed;
  * every key it distributes after encryption;
  * on a reconnect, the Rand and EDIV in its LE Long Term Key Request, which
    is what it presents when it starts encryption from a stored bond;
  * whether the link then encrypts with the key this side holds (the key the
    central stored is right) or drops with 0x3D (it is not).

Like the WBA demo, a Security Request goes out as soon as a central connects.

Typical session: start this, connect from nRF Connect, pair and accept the
number, disconnect, connect again, disconnect, connect a third time. The run
ends after --connections disconnects (default 3) and prints one line for each.

  secure_peripheral_test.py --name BleSecurityPy --connections 3

Options worth knowing:
  --refuse-nonzero   refuse the key request when Rand or EDIV is not zero,
                     as a strict Secure Connections peripheral would
  --bond-file F      keep the bond between runs
  --resp-keys 0x03   offer to distribute keys (nothing is sent; this only
                     changes what the Pairing Response claims)

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
    parse_connection)
from hcicontroller import smp_central as S

from secure_reconnect_test import Clock, Link, reason_text

OP_LE_LTK_REPLY = 0x201A
OP_LE_LTK_NEGATIVE_REPLY = 0x201B


class PeripheralLink(Link):
    """Link that also notices connections and key requests as they arrive."""

    def __init__(self, hci, clock, verbose):
        Link.__init__(self, hci, clock, verbose)
        self.new_conn = None
        self.ltk_requests = []      # (ms, handle, rand, ediv), not yet served
        self.adv_ended = False

    def _on_event(self, code, body):
        now = self.clock.ms()
        if code == EVT_LE_META and body:
            if body[0] == 0x05 and len(body) >= 13:
                handle = struct.unpack("<H", body[1:3])[0]
                rand = bytes(body[3:11])
                ediv = struct.unpack("<H", body[11:13])[0]
                self.ltk_requests.append((now, handle, rand, ediv))
            info = parse_connection(body)
            if info is not None and info[0] == 0 and info[2] == 1:
                status, handle, role, peer, interval, latency, timeout = info
                self.new_conn = (now, handle, body[5], bytes(peer), interval,
                                 latency, timeout)
            if body[0] == 0x12:
                self.adv_ended = True
        Link._on_event(self, code, body)


def start_advertising(hci, name, identity, addr_type, state):
    """Extended advertising when the controller has it, otherwise legacy."""
    handle = 0
    adv = bytes([2, 0x01, 0x06]) + bytes([len(name) + 1, 0x09]) + name

    if state.get("extended") is None:
        params = bytes([handle]) + struct.pack("<H", 0x0013)
        params += (0xA0).to_bytes(3, "little") * 2
        params += bytes([0x07, addr_type, 0x00]) + b"\x00" * 6
        params += bytes([0x00, 0x7F, 0x01, 0x00, 0x01, 0x00, 0x00])
        status, _ = hci.command(T.OP_LE_SET_EXT_ADV_PARAMS, params,
                                allow_fail=True)
        state["extended"] = status == 0
        if state["extended"]:
            if addr_type == 0x01:
                hci.command(T.OP_LE_SET_ADV_SET_RANDOM_ADDR,
                            bytes([handle]) + identity)
            hci.command(T.OP_LE_SET_EXT_ADV_DATA,
                        bytes([handle, 0x03, 0x01, len(adv)]) + adv)
            hci.command(T.OP_LE_SET_EXT_SCAN_RSP_DATA,
                        bytes([handle, 0x03, 0x01, 0]))
        else:
            if addr_type == 0x01:
                hci.command(T.OP_LE_SET_RANDOM_ADDRESS, identity)
            legacy = struct.pack("<HH", 0xA0, 0xA0) + bytes([0x00])
            legacy += bytes([addr_type, 0x00]) + b"\x00" * 6
            legacy += bytes([0x07, 0x00])
            hci.command(T.OP_LE_SET_ADV_PARAMS, legacy)
            hci.command(T.OP_LE_SET_ADV_DATA,
                        bytes([len(adv)]) + adv + b"\x00" * (31 - len(adv)))
            hci.command(T.OP_LE_SET_SCAN_RSP_DATA, bytes(32))

    if state["extended"]:
        hci.command(T.OP_LE_SET_EXT_ADV_ENABLE,
                    bytes([0x01, 0x01, handle]) + struct.pack("<H", 0) +
                    bytes([0]))
    else:
        hci.command(T.OP_LE_SET_ADV_ENABLE, bytes([0x01]))


def load_bonds(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path) as handle:
        raw = json.load(handle)
    return {bytes.fromhex(k): bytes.fromhex(v) for k, v in raw.items()}


def save_bonds(path, bonds):
    if path:
        with open(path, "w") as handle:
            json.dump({k.hex(): v.hex() for k, v in bonds.items()}, handle,
                      indent=2)


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
    link = PeripheralLink(hci, clock, args.raw)
    bonds = load_bonds(args.bond_file)
    summary = []
    adv_state = {}

    try:
        hci.setup()
        override = T.addr_bytes(args.own_address) if args.own_address else None
        own, own_type, source = hci.identity(override)
        clock.say("peripheral address %s (%s)" % (T.addr_str(own), source))
        name = args.name.encode()
        start_advertising(hci, name, own, own_type, adv_state)
        clock.say("advertising as %s; connect from the central now" %
                  args.name)

        deadline = time.monotonic() + args.seconds
        conn = None            # per-connection record
        while time.monotonic() < deadline and len(summary) < args.connections:
            # A new connection.
            if link.new_conn is not None:
                now, handle, peer_type, peer, interval, latency, timeout = \
                    link.new_conn
                link.new_conn = None
                link.handle = handle
                link.disconnect = None
                link.encryption = None
                link.att_seen = 0
                link.reasm = {}
                link.queue = []
                link.ltk_requests = []
                clock.say("connected, handle 0x%04X, central %s (%s), "
                          "interval %.2f ms, latency %d, supervision %d ms" % (
                              handle, T.addr_str(peer),
                              "random" if peer_type & 1 else "public",
                              interval * 1.25, latency, timeout * 10))
                conn = {"handle": handle, "peer": peer, "peer_type": peer_type,
                        "bonded_at_connect": peer in bonds, "smp": None,
                        "paired": False, "key_requests": [], "result": None,
                        "prompted": False, "keys": []}
                clock.say("bond held for this central: %s" %
                          ("yes" if conn["bonded_at_connect"] else "no"))
                if args.secreq_delay:
                    time.sleep(args.secreq_delay / 1000.0)
                link.send_l2cap(S.CID_SMP, bytes((S.SMP_SECURITY_REQUEST,
                                                  args.auth)))
                clock.say("tx Security Request, auth 0x%02X" % args.auth)

            # Key requests: the central starting encryption.
            while link.ltk_requests and conn is not None:
                when, handle, rand, ediv = link.ltk_requests.pop(0)
                zero = rand == bytes(8) and ediv == 0
                clock.say("LE Long Term Key Request: rand %s ediv 0x%04X (%s)" %
                          (rand.hex(), ediv,
                           "both zero" if zero else "NOT zero"))
                conn["key_requests"].append((rand.hex(), ediv))
                smp = conn["smp"]
                key = None
                if smp is not None and smp.ok:
                    key = smp.ltk                   # pairing just finished
                elif conn["peer"] in bonds:
                    key = bonds[conn["peer"]]
                if key is not None and not zero and args.refuse_nonzero:
                    clock.say("refusing: Rand or EDIV is not zero")
                    key = None
                if key is None:
                    hci.command(OP_LE_LTK_NEGATIVE_REPLY,
                                struct.pack("<H", handle), allow_fail=True)
                    clock.say("LTK negative reply (no key for this request)")
                else:
                    hci.command(OP_LE_LTK_REPLY,
                                struct.pack("<H", handle) + key,
                                allow_fail=True)
                    clock.say("LTK reply, key %s" % key.hex())

            # SMP and anything else on the link.
            item = link.poll(0.01)
            if item is not None and conn is not None:
                cid, pdu = item
                if cid == S.CID_SMP and pdu:
                    code = pdu[0]
                    if code == S.SMP_PAIRING_REQUEST:
                        smp = S.SmpResponder(
                            lambda p: link.send_l2cap(S.CID_SMP, p),
                            own, own_type, conn["peer"],
                            1 if conn["peer_type"] & 1 else 0,
                            io_cap=args.io, auth_req=args.auth,
                            key_size=args.key_size,
                            accept_init_keys=args.accept_keys,
                            resp_keys=args.resp_keys, log=clock.say)
                        conn["smp"] = smp
                        conn["prompted"] = False
                        smp.feed(pdu)
                    elif conn["smp"] is not None and code in (
                            S.SMP_PAIRING_PUBLIC_KEY, S.SMP_PAIRING_RANDOM,
                            S.SMP_PAIRING_DHKEY_CHECK, S.SMP_PAIRING_FAILED):
                        conn["smp"].feed(pdu)
                    else:
                        name_ = S.SMP_NAMES.get(code, "0x%02X" % code)
                        clock.say("rx %s %s" % (name_, pdu[1:].hex()))
                        conn["keys"].append(name_)
                else:
                    clock.say("rx cid 0x%04X %s" % (cid, pdu.hex()))

            smp = conn["smp"] if conn else None
            if smp is not None and not conn["prompted"] and \
                    smp.state == S.R_WAIT_USER_OR_CHECK and \
                    smp.method == S.METHOD_NUMERIC and \
                    smp.numeric_value is not None:
                conn["prompted"] = True
                print()
                print("   Numeric value: %06d" % smp.numeric_value)
                if args.auto_accept:
                    print("   Match it against the central and accept there. "
                          "Accepted here automatically.")
                else:
                    input("   Compare with the central, accept there, then "
                          "press Enter here: ")
                print()
                smp.user_confirm(True)
            if smp is not None and smp.ok and not conn["paired"]:
                conn["paired"] = True
                bonds[conn["peer"]] = smp.ltk
                save_bonds(args.bond_file, bonds)
                clock.say("paired and bonded, LTK %s" % smp.ltk.hex())

            # End of a connection.
            if link.disconnect is not None and conn is not None:
                when, status, handle, reason = link.disconnect
                enc = link.encryption
                if enc is not None and enc[1] == 0 and enc[2]:
                    result = "encrypted"
                else:
                    result = "no encryption"
                conn["result"] = "%s; ended %s" % (result, reason_text(reason))
                clock.say("connection ended: %s" % conn["result"])
                summary.append(conn)
                conn = None
                link.handle = None
                link.disconnect = None
                if len(summary) < args.connections:
                    time.sleep(0.2)
                    start_advertising(hci, name, own, own_type, adv_state)
                    clock.say("advertising again")
    except KeyboardInterrupt:
        print()
    finally:
        try:
            if link.handle is not None:
                hci.command(T.OP_DISCONNECT, struct.pack("<HB", link.handle,
                                                         0x13),
                            allow_fail=True)
            if adv_state.get("extended"):
                hci.command(T.OP_LE_SET_EXT_ADV_ENABLE,
                            bytes([0x00, 0x01, 0x00]) + struct.pack("<H", 0)
                            + bytes([0]), allow_fail=True)
            else:
                hci.command(T.OP_LE_SET_ADV_ENABLE, bytes([0x00]),
                            allow_fail=True)
        except Exception:
            pass
        hci.close()

    print()
    print("Summary")
    for index, rec in enumerate(summary, 1):
        smp = rec["smp"]
        line = "  connection %d: %s" % (index, rec["result"])
        if smp is not None and smp.request:
            r = smp.request
            line += "\n      pairing request: io %d auth 0x%02X key size %d " \
                    "keys init 0x%02X resp 0x%02X; %s" % (
                        r["io_cap"], r["auth_req"], r["key_size"],
                        r["init_keys"], r["resp_keys"],
                        "paired" if rec["paired"] else
                        "failed: %s" % smp.failure)
        if rec["key_requests"]:
            line += "\n      key request(s): " + ", ".join(
                "rand %s ediv 0x%04X" % (a, b) for a, b in rec["key_requests"])
        if rec["keys"]:
            line += "\n      also received: " + ", ".join(rec["keys"])
        print(line)
    return 0 if summary else 1


def main():
    p = argparse.ArgumentParser(
        description="Dongle as a bondable LESC peripheral, to log what a "
                    "central does")
    p.add_argument("--name", default="BleSecurityPy")
    p.add_argument("--own-address", help="static random address to advertise "
                                         "with; default is the controller's")
    p.add_argument("--transport", choices=("auto", "serial", "usb"),
                   default="auto")
    p.add_argument("--usb", metavar="SERIAL_OR_VID:PID")
    p.add_argument("--port", help="serial H:4 port instead of discovery")
    p.add_argument("--raw", action="store_true", help="show raw HCI bytes")
    p.add_argument("--connections", type=int, default=3,
                   help="stop after this many connections end (default 3)")
    p.add_argument("--seconds", type=float, default=600.0,
                   help="give up after this long (default 600)")
    p.add_argument("--io", type=int, default=S.IO_DISPLAY_YES_NO)
    p.add_argument("--auth", type=lambda v: int(v, 0), default=0x0D,
                   help="AuthReq for the Security Request and the Pairing "
                        "Response, default 0x0D")
    p.add_argument("--key-size", type=int, default=16)
    p.add_argument("--accept-keys", type=lambda v: int(v, 0), default=0x03,
                   help="initiator keys this side accepts, default 0x03")
    p.add_argument("--resp-keys", type=lambda v: int(v, 0), default=0x00,
                   help="responder keys this side offers, default none")
    p.add_argument("--secreq-delay", type=int, default=0, metavar="MS",
                   help="wait this long before the Security Request")
    p.add_argument("--refuse-nonzero", action="store_true",
                   help="refuse a key request whose Rand or EDIV is not zero")
    p.add_argument("--bond-file", help="keep the bonds in this file")
    p.add_argument("--auto-accept", action="store_true",
                   help="do not wait for Enter at the numeric comparison")
    args = p.parse_args()
    if args.usb and args.port:
        p.error("--usb and --port are mutually exclusive")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())

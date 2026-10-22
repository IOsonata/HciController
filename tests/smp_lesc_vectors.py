#!/usr/bin/env python3
"""Check the LE Secure Connections initiator against the specification.

python/hcicontroller/smp_central.py derives the LTK that a secure reconnect
test hands to LE Enable Encryption. A byte order slip there produces values of
the right length that fail only on a bench, so the toolbox is run here against
the worked examples of Vol 3 Part H Appendix D, and the whole state machine is
run against a responder written to the same flow, with no board attached.

    python3 tests/smp_lesc_vectors.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "python", "hcicontroller"))

import smp_central as S  # noqa: E402

failures = 0


def check(name, got, want):
    global failures
    if got == want:
        print("ok    %s" % name)
    else:
        failures += 1
        print("FAIL  %s\n      got  %s\n      want %s" % (name, got, want))


def wire(text):
    """Specification values are written most significant first."""
    return bytes.fromhex(text)[::-1]


def spec_vectors():
    u = wire("20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6")
    v = wire("55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd")
    x = wire("d5cb8454d177733effffb2ec712baeab")
    y = wire("a6e8e7cc25a75f6e216583f7ff3dc4cf")

    check("f4 (D.2)", S.f4(u, v, x, bytes(1))[::-1].hex(),
          "f2c916f107a9bd1cf1eda1bea974872d")

    w = wire("ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698")
    a1 = wire("0056123737bfce")
    a2 = wire("00a713702dcfc1")
    mac, ltk = S.f5(w, x, y, a1, a2)
    check("f5 MacKey (D.3)", mac[::-1].hex(),
          "2965f176a1084a02fd3f6a20ce636e20")
    check("f5 LTK (D.3)", ltk[::-1].hex(), "6986791169d7cd23980522b594750a38")

    check("f6 (D.4)", S.f6(mac, x, y, wire("12a3343bb453bb5408da42d20c2d0fc8"),
                           wire("010102"), a1, a2)[::-1].hex(),
          "e3c473989cd0e8c5d26c0b09da958f61")

    # D.5 prints the value as the low 32 bits, 0x2f9ed5ba.
    check("g2 (D.5)", S.g2(u, v, x, y), 0x2f9ed5ba % 1000000)


class Responder:
    """The responder half, written from the flow chart in Vol 3 Part H 2.3.5.6."""

    def __init__(self, send, address, address_type, io_cap, auth_req):
        self.send = send
        self.address = address
        self.address_type = address_type
        self.io_cap = io_cap
        self.auth_req = auth_req
        self.keys = S.KeyPair()
        self.nb = os.urandom(16)
        self.pres = None
        self.dhkey = None
        self.initiator = None

    def feed(self, pdu, peer_address, peer_type):
        code = pdu[0]
        if code == S.SMP_PAIRING_REQUEST:
            self.preq = bytes(pdu)
            self.peer_io = pdu[1]
            self.peer_auth = pdu[3]
            self.pres = bytes((S.SMP_PAIRING_RESPONSE, self.io_cap, 0,
                               self.auth_req, 16, 0, 0))
            self.send(self.pres)
        elif code == S.SMP_PAIRING_PUBLIC_KEY:
            self.pka_x, self.pka_y = bytes(pdu[1:33]), bytes(pdu[33:65])
            self.send(bytes((S.SMP_PAIRING_PUBLIC_KEY,)) + self.keys.x +
                      self.keys.y)
            self.dhkey = self.keys.shared_secret(self.pka_x, self.pka_y)
            cb = S.f4(self.keys.x, self.pka_x, self.nb, bytes(1))
            self.send(bytes((S.SMP_PAIRING_CONFIRM,)) + cb)
        elif code == S.SMP_PAIRING_RANDOM:
            self.na = bytes(pdu[1:17])
            self.send(bytes((S.SMP_PAIRING_RANDOM,)) + self.nb)
            self.a1 = S.address_form(peer_address, peer_type)
            self.a2 = S.address_form(self.address, self.address_type)
            self.mac, self.ltk = S.f5(self.dhkey, self.na, self.nb,
                                      self.a1, self.a2)
            self.value = S.g2(self.pka_x, self.keys.x, self.na, self.nb)
        elif code == S.SMP_PAIRING_DHKEY_CHECK:
            ea = S.f6(self.mac, self.na, self.nb, bytes(16),
                      self.preq[1:4], self.a1, self.a2)
            if ea != bytes(pdu[1:17]):
                self.send(bytes((S.SMP_PAIRING_FAILED, 0x0B)))
                return
            eb = S.f6(self.mac, self.nb, self.na, bytes(16),
                      self.pres[1:4], self.a2, self.a1)
            self.send(bytes((S.SMP_PAIRING_DHKEY_CHECK,)) + eb)


def pairing_run(io_cap, expect_method):
    central_addr = bytes.fromhex("1b6889fc71f8")
    peer_addr = bytes.fromhex("112233445566")
    to_peer, to_central = [], []
    central = S.SmpCentral(to_peer.append, central_addr, 1, peer_addr, 0,
                           io_cap=io_cap)
    peer = Responder(to_central.append, peer_addr, 0, io_cap, 0x0D)
    central.auth_req = 0x0D

    central.start()
    for _ in range(40):
        while to_peer:
            peer.feed(to_peer.pop(0), central_addr, 1)
        while to_central:
            central.feed(to_central.pop(0))
        if central.state == S.WAIT_USER:
            check("numeric value agrees", central.numeric_value, peer.value)
            central.user_confirm(True)
        if central.done and not to_peer:
            break
    check("method %s" % expect_method, central.method, expect_method)
    check("pairing completes", central.ok, True)
    if central.ok:
        check("both sides hold one LTK", central.ltk, peer.ltk)


def responder_run(io_cap, expect_method):
    """SmpCentral against SmpResponder, the pair the two hardware tests use."""
    central_addr = bytes.fromhex("1b6889fc71f8")
    peer_addr = bytes.fromhex("665544332211")
    to_resp, to_cent = [], []
    cent = S.SmpCentral(to_resp.append, central_addr, 1, peer_addr, 0,
                        io_cap=io_cap)
    resp = S.SmpResponder(to_cent.append, peer_addr, 0, central_addr, 1,
                          io_cap=io_cap)
    cent.start()
    for _ in range(60):
        while to_resp:
            resp.feed(to_resp.pop(0))
        while to_cent:
            cent.feed(to_cent.pop(0))
        if cent.state == S.WAIT_USER:
            check("responder: same number", cent.numeric_value,
                  resp.numeric_value)
            cent.user_confirm(True)
        if resp.state == S.R_WAIT_USER_OR_CHECK and \
                resp.user_accepted is None and resp.numeric_value is not None:
            resp.user_confirm(True)
        if cent.done and resp.done:
            break
    check("responder: method %s" % expect_method, resp.method, expect_method)
    check("responder: both complete", (cent.ok, resp.ok), (True, True))
    check("responder: one LTK", cent.ltk, resp.ltk)


def refusals():
    central_addr = bytes.fromhex("1b6889fc71f8")
    peer_addr = bytes.fromhex("112233445566")
    out = []
    c = S.SmpCentral(out.append, central_addr, 1, peer_addr, 0)
    c.start()
    # Legacy answer: SC bit absent.
    c.feed(bytes((S.SMP_PAIRING_RESPONSE, 1, 0, 0x05, 16, 0, 0)))
    check("legacy answer is refused by name", c.ok is False and
          "legacy" in (c.failure or ""), True)

    out = []
    c = S.SmpCentral(out.append, central_addr, 1, peer_addr, 0)
    c.start()
    c.feed(bytes((S.SMP_PAIRING_FAILED, 0x05)))
    check("peer Pairing Failed is recorded", c.peer_failure, 0x05)

    # A confirm that does not match the random must stop the run.
    to_peer, to_central = [], []
    c = S.SmpCentral(to_peer.append, central_addr, 1, peer_addr, 0)
    p = Responder(to_central.append, peer_addr, 0, 1, 0x0D)
    c.start()
    for _ in range(40):
        while to_peer:
            p.feed(to_peer.pop(0), central_addr, 1)
        while to_central:
            pdu = to_central.pop(0)
            if pdu[0] == S.SMP_PAIRING_CONFIRM:
                pdu = bytes((pdu[0],)) + bytes(16)
            c.feed(pdu)
        if c.done:
            break
    check("bad confirm fails pairing", (c.ok, c.peer_failure), (False, None))
    check("bad confirm answered Confirm Value Failed", to_peer[-1:] ==
          [bytes((S.SMP_PAIRING_FAILED, 0x04))] or
          bytes((S.SMP_PAIRING_FAILED, 0x04)) in to_peer, True)


def main():
    spec_vectors()
    pairing_run(S.IO_DISPLAY_YES_NO, S.METHOD_NUMERIC)
    pairing_run(S.IO_NO_INPUT_NO_OUTPUT, S.METHOD_JUST_WORKS)
    responder_run(S.IO_DISPLAY_YES_NO, S.METHOD_NUMERIC)
    responder_run(S.IO_NO_INPUT_NO_OUTPUT, S.METHOD_JUST_WORKS)
    refusals()
    print()
    print("%s" % ("all checks passed" if not failures
                  else "%d checks failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

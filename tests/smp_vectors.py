#!/usr/bin/env python3
"""Check the pairing crypto against the specification's own test vectors.

python/hcicontroller/hci_ble_test.py implements enough of the Security Manager to
let a phone pair Just Works, so that link layer encryption gets exercised and
the two Long Term Key Request commands answer a real request instead of being
checked for their shape. The confirm value c1 and the key derivation s1 are
the part of that which can be silently wrong: every field is on the wire
least significant octet first and the specification writes them the other way
round, so an implementation with the order reversed produces values that are
the right length, the right type, and useless. Nothing reports it except a
pairing that does not complete, on someone's bench, later.

The Bluetooth specification publishes worked examples for both, Vol 3 Part H
Appendix D. This runs them.

AES is here in full rather than through the controller, because the point is
to check this code with no board attached. It is checked against the
FIPS 197 example first, so a fault in the cipher cannot be read as a fault in
c1.

    python3 tests/smp_vectors.py
"""

import os
import sys


def build_sbox():
    """The AES substitution box, generated rather than typed out.

    256 octets copied by hand is 256 chances to make a mistake that looks
    like a pairing failure. Generating it from the affine transform over
    GF(2^8) is the same table and can be checked against two known entries.
    """
    sbox = [0] * 256
    p = 1
    q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= (q << 1) & 0xFF
        q ^= (q << 2) & 0xFF
        q ^= (q << 4) & 0xFF
        if q & 0x80:
            q ^= 0x09
        q &= 0xFF
        value = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) \
            ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (value ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


SBOX = build_sbox()
RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def xtime(value):
    value <<= 1
    if value & 0x100:
        value ^= 0x11B
    return value & 0xFF


def expand_key(key):
    words = [list(key[i * 4:i * 4 + 4]) for i in range(4)]
    for i in range(4, 44):
        temp = list(words[i - 1])
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [SBOX[b] for b in temp]
            temp[0] ^= RCON[i // 4 - 1]
        words.append([a ^ b for a, b in zip(words[i - 4], temp)])
    return words


def aes128(key, block):
    """One AES-128 block, most significant octet first, as FIPS 197 has it."""
    words = expand_key(key)

    def add_round_key(state, rnd):
        for col in range(4):
            for row in range(4):
                state[row][col] ^= words[rnd * 4 + col][row]

    state = [[block[row + 4 * col] for col in range(4)] for row in range(4)]
    add_round_key(state, 0)

    for rnd in range(1, 11):
        for row in range(4):
            for col in range(4):
                state[row][col] = SBOX[state[row][col]]
        for row in range(1, 4):
            state[row] = state[row][row:] + state[row][:row]
        if rnd != 10:
            for col in range(4):
                a = [state[row][col] for row in range(4)]
                total = a[0] ^ a[1] ^ a[2] ^ a[3]
                first = a[0]
                for row in range(4):
                    nxt = a[(row + 1) % 4] if row != 3 else first
                    state[row][col] = a[row] ^ total ^ xtime(a[row] ^ nxt)
        add_round_key(state, rnd)

    return bytes(state[row % 4][row // 4] for row in
                 [c * 4 + r for c in range(4) for r in range(4)])


class FakeHci:
    """Answers LE Encrypt with a real AES block and nothing else."""

    def command(self, opcode, payload=b"", timeout=3.0, allow_fail=False):
        assert opcode == 0x2017, "only LE Encrypt is answered here"
        return 0, aes128(payload[:16], payload[16:32])


def check(label, got, want):
    if got == want:
        print("[ok] %-34s %s" % (label, got.hex()))
        return 0
    print("[!!] %-34s %s" % (label, got.hex()))
    print("     the specification says              %s" % want.hex())
    return 1


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    sys.path.insert(0, os.path.join(repo_root, "python"))
    try:
        import hcicontroller.hci_ble_test as hci_ble_test
    except SystemExit:
        # The tool exits when pyserial is absent, which has nothing to do
        # with the crypto in it.
        print("pyserial is missing, so hcicontroller.hci_ble_test cannot be imported.")
        print("Run: pip3 install pyserial")
        return 0

    failures = 0

    # FIPS 197 C.1, so a wrong cipher is not read as a wrong c1 below.
    failures += check(
        "AES-128, FIPS 197 example",
        aes128(bytes.fromhex("000102030405060708090a0b0c0d0e0f"),
               bytes.fromhex("00112233445566778899aabbccddeeff")),
        bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a"))

    # Vol 3 Part H D.1. Same block, reached the way the pairing code reaches
    # it, so the reversing in SmpPeripheral.encrypt is checked too.
    smp = hci_ble_test.SmpPeripheral(FakeHci(), 0x0001, bytes(6), 0,
                                     bytes(6), 0)
    failures += check(
        "e, Vol 3 Part H D.1",
        smp.encrypt(bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")[::-1],
                    bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")[::-1]),
        bytes.fromhex("3ad77bb40d7a3660a89ecaf32466ef97")[::-1])

    # Vol 3 Part H D.2. Everything reversed on the way in, because the
    # specification writes these most significant octet first and every one
    # of them is on the wire the other way round.
    smp = hci_ble_test.SmpPeripheral(
        FakeHci(), 0x0001,
        bytes.fromhex("b1b2b3b4b5b6")[::-1], 0x00,
        bytes.fromhex("a1a2a3a4a5a6")[::-1], 0x01)
    smp.preq = bytes.fromhex("07071000000101")[::-1]
    smp.pres = bytes.fromhex("05000800000302")[::-1]
    failures += check(
        "c1, Vol 3 Part H D.2",
        smp.c1(bytes(16),
               bytes.fromhex("5783d52156ad6f0e6388274ec6702ee0")[::-1]),
        bytes.fromhex("1e1e3fef878988ead2a74dc5bef13b86")[::-1])

    # Vol 3 Part H D.3. s1(k, r1, r2) with r1 the responder's random and r2
    # the initiator's, which is the order the peripheral calls it in.
    failures += check(
        "s1, Vol 3 Part H D.3",
        smp.s1(bytes(16),
               bytes.fromhex("000f0e0d0c0b0a091122334455667788")[::-1],
               bytes.fromhex("010203040506070899aabbccddeeff00")[::-1]),
        bytes.fromhex("9a1fe1f0e8b0f49b5b4216ae796da062")[::-1])

    print()
    if failures:
        print("%d pairing vector(s) disagree with the specification. A "
              "phone will" % failures)
        print("not pair with this, and the only symptom on a bench is a "
              "pairing that")
        print("does not complete.")
        return 1

    print("All pairing crypto vectors passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
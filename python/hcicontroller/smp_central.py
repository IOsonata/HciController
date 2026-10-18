#!/usr/bin/env python3
"""
LE Secure Connections pairing, initiator side, with no I/O of its own.

The class here is a state machine. The caller hands it every SMP PDU that
arrives on L2CAP channel 6 and sends whatever the send callback is given. It
opens no port and imports no serial module, so a test can run it against a
recorded peer or another Security Manager with no board attached.

What it covers:

  * Pairing Request / Response, with the peer's choice of Secure Connections
    or legacy visible in the result (this stops with a clear message on
    legacy rather than guessing at c1 and s1).
  * Public key exchange on P-256 and the DHKey.
  * Just Works and Numeric Comparison. Passkey Entry is refused by name.
  * f4 confirm check, g2 numeric value, f5 key generation, f6 check values.
  * The LTK in the byte order LE Enable Encryption takes.

Byte order is the usual trap. Every field on the wire is least significant
octet first and the specification writes the toolbox functions most
significant first. All values handled here are the wire form; the functions
reverse internally. The module is checked against the specification's own
vectors in tests/smp_lesc_vectors.py and against Bumble.

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
"""

import os

try:
    from cryptography.hazmat.primitives import cmac
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.ciphers import algorithms
except ImportError as err:
    raise ImportError(
        "smp_central needs the cryptography package for P-256 and AES-CMAC. "
        "Install it with: python3 -m pip install cryptography, or reinstall "
        "the library with: python3 -m pip install -e ./python") from err

CID_SMP = 0x0006

# Vol 3 Part H 3.3.
SMP_PAIRING_REQUEST = 0x01
SMP_PAIRING_RESPONSE = 0x02
SMP_PAIRING_CONFIRM = 0x03
SMP_PAIRING_RANDOM = 0x04
SMP_PAIRING_FAILED = 0x05
SMP_ENCRYPTION_INFO = 0x06
SMP_CENTRAL_IDENTIFICATION = 0x07
SMP_IDENTITY_INFO = 0x08
SMP_IDENTITY_ADDRESS_INFO = 0x09
SMP_SIGNING_INFO = 0x0A
SMP_SECURITY_REQUEST = 0x0B
SMP_PAIRING_PUBLIC_KEY = 0x0C
SMP_PAIRING_DHKEY_CHECK = 0x0D
SMP_KEYPRESS_NOTIFICATION = 0x0E

SMP_NAMES = {
    SMP_PAIRING_REQUEST: "Pairing Request",
    SMP_PAIRING_RESPONSE: "Pairing Response",
    SMP_PAIRING_CONFIRM: "Pairing Confirm",
    SMP_PAIRING_RANDOM: "Pairing Random",
    SMP_PAIRING_FAILED: "Pairing Failed",
    SMP_ENCRYPTION_INFO: "Encryption Information",
    SMP_CENTRAL_IDENTIFICATION: "Central Identification",
    SMP_IDENTITY_INFO: "Identity Information",
    SMP_IDENTITY_ADDRESS_INFO: "Identity Address Information",
    SMP_SIGNING_INFO: "Signing Information",
    SMP_SECURITY_REQUEST: "Security Request",
    SMP_PAIRING_PUBLIC_KEY: "Pairing Public Key",
    SMP_PAIRING_DHKEY_CHECK: "Pairing DHKey Check",
    SMP_KEYPRESS_NOTIFICATION: "Keypress Notification",
}

# Vol 3 Part H 3.5.5.
FAILED_NAMES = {
    0x01: "Passkey Entry Failed",
    0x02: "OOB Not Available",
    0x03: "Authentication Requirements",
    0x04: "Confirm Value Failed",
    0x05: "Pairing Not Supported",
    0x06: "Encryption Key Size",
    0x07: "Command Not Supported",
    0x08: "Unspecified Reason",
    0x09: "Repeated Attempts",
    0x0A: "Invalid Parameters",
    0x0B: "DHKey Check Failed",
    0x0C: "Numeric Comparison Failed",
    0x0D: "BR/EDR Pairing In Progress",
    0x0E: "Cross-transport Key Derivation Not Allowed",
}

IO_DISPLAY_ONLY = 0x00
IO_DISPLAY_YES_NO = 0x01
IO_KEYBOARD_ONLY = 0x02
IO_NO_INPUT_NO_OUTPUT = 0x03
IO_KEYBOARD_DISPLAY = 0x04

AUTH_BONDING = 0x01
AUTH_MITM = 0x04
AUTH_SC = 0x08

METHOD_JUST_WORKS = "Just Works"
METHOD_NUMERIC = "Numeric Comparison"
METHOD_PASSKEY = "Passkey Entry"

# Vol 3 Part H Table 2.8, Secure Connections. Row is the initiator's IO
# capability, column the responder's, both in the order of the IO_ constants.
_JW, _NC, _PE = METHOD_JUST_WORKS, METHOD_NUMERIC, METHOD_PASSKEY
_SC_METHOD = (
    (_JW, _JW, _PE, _JW, _PE),
    (_JW, _NC, _PE, _JW, _NC),
    (_PE, _PE, _PE, _JW, _PE),
    (_JW, _JW, _JW, _JW, _JW),
    (_PE, _NC, _PE, _JW, _NC),
)


def rev(data):
    return bytes(reversed(data))


# ---------------------------------------------------------------------------
# The toolbox. Arguments and results are wire order (least significant first).
# ---------------------------------------------------------------------------
def aes_cmac(key, message):
    """AES-128 CMAC, both arguments most significant first as in RFC 4493."""
    engine = cmac.CMAC(algorithms.AES(key))
    engine.update(message)
    return engine.finalize()


def f4(u, v, x, z):
    """Vol 3 Part H 2.2.6. u and v are the public key X coordinates."""
    return rev(aes_cmac(rev(x), rev(u) + rev(v) + z))


_SALT = bytes.fromhex("6C888391AAF5A53860370BDB5A6083BE")
_KEY_ID = bytes((0x62, 0x74, 0x6C, 0x65))


def f5(w, n1, n2, a1, a2):
    """
    Vol 3 Part H 2.2.7. Returns (MacKey, LTK) in wire order. a1 and a2 are the
    seven octet address forms, six address octets then the type octet, which
    is the wire order of an address followed by its type.
    """
    t = aes_cmac(_SALT, rev(w))

    def block(counter):
        return aes_cmac(t, bytes((counter,)) + _KEY_ID + rev(n1) + rev(n2) +
                        rev(a1) + rev(a2) + bytes((0x01, 0x00)))

    return rev(block(0)), rev(block(1))


def f6(w, n1, n2, r, io_cap, a1, a2):
    """Vol 3 Part H 2.2.8. io_cap is AuthReq, OOB, IO capability."""
    return rev(aes_cmac(rev(w), rev(n1) + rev(n2) + rev(r) + rev(io_cap) +
                        rev(a1) + rev(a2)))


def g2(u, v, x, y):
    """Vol 3 Part H 2.2.9. The six digit value both sides display."""
    value = aes_cmac(rev(x), rev(u) + rev(v) + rev(y))
    return int.from_bytes(value[-4:], "big") % 1000000


def address_form(address, address_type):
    """Seven octet form f5 and f6 take: wire address, then its type octet."""
    return bytes(address) + bytes((address_type,))


# ---------------------------------------------------------------------------
# P-256
# ---------------------------------------------------------------------------
class KeyPair:
    def __init__(self, private=None):
        self.private = private or ec.generate_private_key(ec.SECP256R1())
        numbers = self.private.public_key().public_numbers()
        # Wire order, least significant first.
        self.x = numbers.x.to_bytes(32, "big")[::-1]
        self.y = numbers.y.to_bytes(32, "big")[::-1]

    def shared_secret(self, peer_x, peer_y):
        """
        DHKey in wire order. Raises ValueError when the peer point is not on
        the curve, which is what the specification calls an invalid public
        key and answers with Pairing Failed, Invalid Parameters.
        """
        point = ec.EllipticCurvePublicNumbers(
            int.from_bytes(peer_x, "little"),
            int.from_bytes(peer_y, "little"), ec.SECP256R1())
        public = point.public_key()
        secret = self.private.exchange(ec.ECDH(), public)
        return rev(secret)


class PairingError(Exception):
    pass


# States
IDLE, WAIT_RESPONSE, WAIT_PUBLIC_KEY, WAIT_CONFIRM, WAIT_RANDOM, \
    WAIT_USER, WAIT_DHKEY_CHECK, COMPLETE, FAILED = range(9)

STATE_NAMES = ("idle", "wait response", "wait public key", "wait confirm",
               "wait random", "wait user", "wait dhkey check", "complete",
               "failed")


class SmpCentral:
    """
    Initiator of an LE Secure Connections pairing.

    send(payload)       called with each SMP PDU to transmit
    log(text)           optional, one line per step
    rng(n)              random source, replaceable for a repeatable run
    """

    def __init__(self, send, local_address, local_type, peer_address,
                 peer_type, io_cap=IO_DISPLAY_YES_NO,
                 auth_req=AUTH_BONDING | AUTH_MITM | AUTH_SC,
                 key_size=16, init_keys=0x00, resp_keys=0x00,
                 log=None, rng=os.urandom, keypair=None):
        self.send = send
        self.log = log or (lambda text: None)
        self.rng = rng
        self.local_address = bytes(local_address)
        self.local_type = local_type
        self.peer_address = bytes(peer_address)
        self.peer_type = peer_type
        self.io_cap = io_cap
        self.auth_req = auth_req
        self.key_size = key_size
        self.init_keys = init_keys
        self.resp_keys = resp_keys
        self.keypair = keypair or KeyPair()

        self.state = IDLE
        self.method = None
        self.failure = None        # text, set when state is FAILED
        self.peer_failure = None   # reason code, when the peer sent one
        self.numeric_value = None  # the six digits to compare
        self.ltk = None            # wire order, what LE Enable Encryption takes
        self.mac_key = None
        self.dhkey = None
        self.preq = None
        self.pres = None
        self.peer_io_cap = None
        self.peer_auth_req = None
        self.na = None
        self.nb = None
        self.peer_x = None
        self.peer_y = None
        self.confirm = None
        self.dhkey_check = None
        self.user_accepted = None

    # -- helpers ----------------------------------------------------------
    @property
    def done(self):
        return self.state in (COMPLETE, FAILED)

    @property
    def ok(self):
        return self.state == COMPLETE

    def _a1(self):
        return address_form(self.local_address, self.local_type)

    def _a2(self):
        return address_form(self.peer_address, self.peer_type)

    def _fail(self, text, reason=None):
        self.failure = text
        self.state = FAILED
        self.log("pairing failed: %s" % text)
        if reason is not None:
            self.send(bytes((SMP_PAIRING_FAILED, reason)))

    def _emit(self, pdu):
        self.log("tx %s" % SMP_NAMES.get(pdu[0], "0x%02X" % pdu[0]))
        self.send(bytes(pdu))

    # -- entry points -----------------------------------------------------
    def start(self):
        if self.state != IDLE:
            raise PairingError("pairing already started")
        self.preq = bytes((SMP_PAIRING_REQUEST, self.io_cap, 0x00,
                           self.auth_req, self.key_size, self.init_keys,
                           self.resp_keys))
        self.state = WAIT_RESPONSE
        self._emit(self.preq)

    def feed(self, pdu):
        """Hand over one reassembled SMP PDU from the peer."""
        if self.done or not pdu:
            return
        code = pdu[0]
        self.log("rx %s%s" % (SMP_NAMES.get(code, "0x%02X" % code),
                              " " + pdu[1:].hex() if len(pdu) > 1 else ""))

        if code == SMP_PAIRING_FAILED:
            self.peer_failure = pdu[1] if len(pdu) > 1 else None
            self._fail("peer sent Pairing Failed, %s" % FAILED_NAMES.get(
                self.peer_failure, "reason 0x%02X" % (self.peer_failure or 0)))
            return

        handler = {
            SMP_PAIRING_RESPONSE: self._on_response,
            SMP_PAIRING_PUBLIC_KEY: self._on_public_key,
            SMP_PAIRING_CONFIRM: self._on_confirm,
            SMP_PAIRING_RANDOM: self._on_random,
            SMP_PAIRING_DHKEY_CHECK: self._on_dhkey_check,
        }.get(code)
        if handler is None:
            self.log("ignored, nothing waits for it in state %s" %
                     STATE_NAMES[self.state])
            return
        try:
            handler(pdu)
        except PairingError as err:
            self._fail(str(err), 0x0A)

    def user_confirm(self, accepted):
        """The answer to the numeric comparison, from whoever looked at it."""
        if self.state != WAIT_USER:
            raise PairingError("no comparison is waiting")
        self.user_accepted = accepted
        if not accepted:
            self._fail("numeric comparison rejected here", 0x0C)
            return
        self._send_dhkey_check()

    # -- steps ------------------------------------------------------------
    def _on_response(self, pdu):
        if self.state != WAIT_RESPONSE:
            raise PairingError("unexpected Pairing Response")
        if len(pdu) != 7:
            raise PairingError("Pairing Response is %d octets" % len(pdu))
        self.pres = bytes(pdu)
        self.peer_io_cap = pdu[1]
        self.peer_auth_req = pdu[3]
        peer_key_size = pdu[4]
        self.log("peer io cap %d, oob %d, auth 0x%02X, key size %d, "
                 "keys init 0x%02X resp 0x%02X" % (
                     pdu[1], pdu[2], pdu[3], pdu[4], pdu[5], pdu[6]))

        if not (self.auth_req & AUTH_SC and self.peer_auth_req & AUTH_SC):
            self._fail("the peer answered without Secure Connections, so "
                       "this is legacy pairing, which this initiator does "
                       "not do", 0x05)
            return
        if pdu[2] != 0:
            self._fail("the peer offers out of band data", 0x02)
            return
        if peer_key_size < 7 or peer_key_size > 16:
            self._fail("key size %d" % peer_key_size, 0x06)
            return
        if self.io_cap > 4 or self.peer_io_cap > 4:
            self._fail("io capability out of range", 0x0A)
            return

        mitm = bool(self.auth_req & AUTH_MITM or self.peer_auth_req & AUTH_MITM)
        self.method = _SC_METHOD[self.io_cap][self.peer_io_cap] if mitm \
            else METHOD_JUST_WORKS
        self.log("method: %s" % self.method)
        if self.method == METHOD_PASSKEY:
            self._fail("Passkey Entry is not implemented here; use io cap 1 "
                       "(display yes/no) or 3 (no input no output)", 0x05)
            return

        self.state = WAIT_PUBLIC_KEY
        self._emit(bytes((SMP_PAIRING_PUBLIC_KEY,)) + self.keypair.x +
                   self.keypair.y)

    def _on_public_key(self, pdu):
        if self.state != WAIT_PUBLIC_KEY:
            raise PairingError("unexpected Pairing Public Key")
        if len(pdu) != 65:
            raise PairingError("Pairing Public Key is %d octets" % len(pdu))
        self.peer_x = bytes(pdu[1:33])
        self.peer_y = bytes(pdu[33:65])
        if self.peer_x == self.keypair.x and self.peer_y == self.keypair.y:
            raise PairingError("peer returned our own public key")
        try:
            self.dhkey = self.keypair.shared_secret(self.peer_x, self.peer_y)
        except ValueError:
            raise PairingError("peer public key is not on the curve")
        self.log("dhkey computed")
        self.state = WAIT_CONFIRM

    def _on_confirm(self, pdu):
        if self.state != WAIT_CONFIRM:
            raise PairingError("unexpected Pairing Confirm")
        if len(pdu) != 17:
            raise PairingError("Pairing Confirm is %d octets" % len(pdu))
        self.confirm = bytes(pdu[1:17])
        self.na = self.rng(16)
        self.state = WAIT_RANDOM
        self._emit(bytes((SMP_PAIRING_RANDOM,)) + self.na)

    def _on_random(self, pdu):
        if self.state != WAIT_RANDOM:
            raise PairingError("unexpected Pairing Random")
        if len(pdu) != 17:
            raise PairingError("Pairing Random is %d octets" % len(pdu))
        self.nb = bytes(pdu[1:17])

        # The responder committed to Nb before it saw Na. Both are zero for
        # the z of Numeric Comparison and Just Works.
        expected = f4(self.peer_x, self.keypair.x, self.nb, bytes(1))
        if expected != self.confirm:
            self._fail("confirm value check failed: the peer's Pairing "
                       "Confirm does not match f4 of its random", 0x04)
            return
        self.log("confirm value verified")

        self.mac_key, self.ltk = f5(self.dhkey, self.na, self.nb,
                                    self._a1(), self._a2())
        self.log("ltk %s" % self.ltk.hex())

        if self.method == METHOD_NUMERIC:
            self.numeric_value = g2(self.keypair.x, self.peer_x,
                                    self.na, self.nb)
            self.state = WAIT_USER
            self.log("numeric value %06d" % self.numeric_value)
            return
        self._send_dhkey_check()

    def _io_cap_bytes(self, auth_req, io_cap):
        # AuthReq, OOB, IO capability, the field order of f6.
        return bytes((auth_req, 0x00, io_cap))

    def _send_dhkey_check(self):
        ea = f6(self.mac_key, self.na, self.nb, bytes(16),
                self._io_cap_bytes(self.auth_req, self.io_cap),
                self._a1(), self._a2())
        # Where the peer's value is checked: the initiator's check goes
        # first, the responder's comes back after it.
        self.dhkey_check = f6(self.mac_key, self.nb, self.na, bytes(16),
                              self._io_cap_bytes(self.peer_auth_req,
                                                 self.peer_io_cap),
                              self._a2(), self._a1())
        self.state = WAIT_DHKEY_CHECK
        self._emit(bytes((SMP_PAIRING_DHKEY_CHECK,)) + ea)

    def _on_dhkey_check(self, pdu):
        if self.state != WAIT_DHKEY_CHECK:
            raise PairingError("unexpected Pairing DHKey Check")
        if len(pdu) != 17:
            raise PairingError("Pairing DHKey Check is %d octets" % len(pdu))
        if bytes(pdu[1:17]) != self.dhkey_check:
            self._fail("the peer's DHKey Check does not match", 0x0B)
            return
        self.log("dhkey check verified, pairing complete")
        self.state = COMPLETE

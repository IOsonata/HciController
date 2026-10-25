"""Bluetooth 5.4 advertising acceptance procedures (two controllers).

Core Vol 4 Part E 7.8.53 and 7.7.65.13: command PHY 3 means
LE Coded; report PHY 4 means S=2 only after host feature bit 41 is set.
EAD is opaque to the controller; cryptographic tests live in IOsonata.
"""

import secrets
import struct
import time

from . import periodic_features as p
from .hci_pair import prepare_controller
from .profile import read_controller_capabilities

OP_EXT_ADV_PARAMS_V2 = 0x207F
OP_SET_HOST_FEATURE = 0x2074
CORE54_VERSION = 0x0D

REQUIRED_FEATURES = (
    (40, 'Advertising Coding Selection'),
    (43, 'PAwR Advertiser'),
    (44, 'PAwR Scanner'),
)
REQUIRED_COMMANDS = (
    (44, 1, 0x2074, 'LE Set Host Feature'),
    (46, 2, 0x207F, 'LE Set Extended Advertising Parameters v2'),
    (46, 5, 0x2082, 'LE Set Periodic Advertising Subevent Data'),
    (46, 6, 0x2083, 'LE Set Periodic Advertising Response Data'),
    (46, 7, 0x2084, 'LE Set Periodic Sync Subevent'),
    (47, 0, 0x2085, 'LE Extended Create Connection v2'),
    (47, 1, 0x2086, 'LE Set Periodic Advertising Parameters v2'),
)

# CSS Part A 2.3.1 sample: randomizer, ciphertext, MIC. Public test data only.
EAD_SAMPLE = bytes.fromhex(
    '18e157cade74e4dcafdc51c7282810c2217f0e4cef4343181fba0069cc')


def profile54_errors(capabilities):
    """Return release errors for the controller-side Bluetooth 5.4 additions."""
    errors = []
    version = capabilities.get('version', b'')
    commands = capabilities.get('commands', b'')
    features = capabilities.get('features', b'')

    if (len(version) < 8 or version[0] < CORE54_VERSION
            or version[3] < CORE54_VERSION):
        errors.append('Core version is below 5.4: ' + version.hex(' '))

    if len(features) != 8:
        errors.append('LE Supported Features is %u octets, expected 8'
                      % len(features))
    else:
        for bit, name in REQUIRED_FEATURES:
            if not (features[bit // 8] & (1 << (bit % 8))):
                errors.append('%s feature bit %u is clear' % (name, bit))

    if len(commands) != 64:
        errors.append('Supported Commands is %u octets, expected 64'
                      % len(commands))
    else:
        for octet, bit, opcode, name in REQUIRED_COMMANDS:
            if not (commands[octet] & (1 << bit)):
                errors.append('%s (0x%04X) missing from Supported Commands'
                              % (name, opcode))

    return errors


def run_profile54_phase(book, controller_a, controller_b):
    """Check the advertised HCI profile for the four scoped 5.4 additions."""
    for label, hci in (('A', controller_a), ('B', controller_b)):
        try:
            capabilities = read_controller_capabilities(hci)
            errors = profile54_errors(capabilities)
            if errors:
                book.failed('Bluetooth 5.4 profile', label, '; '.join(errors))
            else:
                book.passed('Bluetooth 5.4 profile', label)
        except Exception as err:
            book.failed('Bluetooth 5.4 profile', label, str(err))


def advertising_params(addr_type, option):
    payload = bytearray(p._ext_adv_params(0, addr_type, 0, 0))
    payload[20] = payload[22] = 3 if option else 1
    return bytes(payload) + bytes([option, option])


def reports(body):
    """Decode complete report records, rejecting truncation and trailing data."""
    if not body or body[0] != p.LE_EXTENDED_ADVERTISING_REPORT:
        return []
    if len(body) < 2:
        raise p.HciError('short extended advertising event')
    result = []
    at = 2
    for _ in range(body[1]):
        if len(body) < at + 24:
            raise p.HciError('short extended advertising report')
        end = at + 24 + body[at + 23]
        if end > len(body):
            raise p.HciError('truncated extended advertising data')
        result.append((int.from_bytes(body[at:at + 2], 'little'),
                       body[at + 2], body[at + 3:at + 9],
                       body[at + 9], body[at + 10], body[at + 11],
                       body[at + 24:end]))
        at = end
    if at != len(body):
        raise p.HciError('trailing extended advertising data')
    return result


def observe(scanner, address, addr_type, payload, phy, timeout=8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        packet = scanner.read_packet(min(0.2, max(0, deadline - time.monotonic())))
        if packet is None or packet[:2] != (p.H4_EVENT, p.EVT_LE_META):
            continue
        for flags, kind, addr, primary, secondary, sid, data in reports(packet[2]):
            if (kind, addr, sid) != (addr_type, address, 0) or data != payload:
                continue
            if flags & 0x60:
                raise p.HciError('advertising data was fragmented or truncated')
            if (primary, secondary) != (phy, phy):
                raise p.HciError('unexpected advertising PHY: %u/%u, expected %u'
                                 % (primary, secondary, phy))
            return
    raise p.HciError('no matching complete advertisement from the selected peer')


def run_advertising54_phase(book, label, advertiser, scanner):
    # Required coding (3/4), rather than preferred coding (1/2), makes the
    # observed S value a deterministic assertion. Unsupported requests fail.
    cases = ((0, True, 1, 'EAD opaque transport / 1M'),
             (3, True, 4, 'required S=2 / host reporting enabled'),
             (4, True, 3, 'required S=8 / host reporting enabled'),
             (3, False, 3, 'S=2 / legacy coded reporting'))
    for option, host_support, phy, name in cases:
        try:
            address, addr_type, _ = prepare_controller(advertiser)
            _, scan_type, _ = prepare_controller(scanner)
            p._command_ok(scanner, OP_SET_HOST_FEATURE, bytes([41, host_support]))
            p._command_ok(advertiser, OP_EXT_ADV_PARAMS_V2,
                          advertising_params(addr_type, option))
            if addr_type == 1:
                p._command_ok(advertiser, p.OP_LE_SET_ADV_SET_RANDOM_ADDRESS,
                              b'\x00' + address)
            # Fresh local-name token prevents stale reports satisfying a case.
            token = b'HCI54-' + secrets.token_hex(4).encode('ascii')
            data = bytes([len(token) + 1, 0x09]) + token
            data += bytes([len(EAD_SAMPLE) + 1, 0x31]) + EAD_SAMPLE
            p._command_ok(advertiser, p.OP_LE_SET_EXT_ADV_DATA,
                          bytes([0, 3, 1, len(data)]) + data)
            scan = bytes([scan_type, 0, 4 if option else 1, 0])
            scan += struct.pack('<HH', 0x60, 0x60)
            p._command_ok(scanner, p.OP_LE_SET_EXT_SCAN_PARAMS, scan)
            p._set_sync_scan(scanner, True)
            p._ext_adv_enable(advertiser, 0)
            observe(scanner, address, addr_type, data, phy)
            book.passed('Bluetooth 5.4 ' + label, name)
        except Exception as err:
            book.failed('Bluetooth 5.4 ' + label, name, str(err))
        finally:
            # Reset also clears host support and per-set coding. A failed
            # cleanup is a release failure, never silently swallowed.
            for role, hci in (('advertiser', advertiser), ('scanner', scanner)):
                try:
                    p._command_ok(hci, 0x0C03)
                except Exception as err:
                    book.failed('Bluetooth 5.4 ' + label,
                                name + ' / ' + role + ' reset', str(err))

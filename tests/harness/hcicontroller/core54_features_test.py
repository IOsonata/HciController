#!/usr/bin/env python3
"""Host regressions for the 5.4 acceptance procedure and result accounting."""
import unittest
from unittest.mock import patch

import _bootstrap  # noqa: F401
from hcicontroller import core54_features as c
from hcicontroller.results import ResultBook

ADDR = bytes.fromhex('0102030405c6')


def report(data, primary=4, secondary=4, flags=0, addr=ADDR):
    fixed = (flags.to_bytes(2, 'little') + b'\x01' + addr
             + bytes([primary, secondary, 0, 0, 0]) + bytes(9)
             + bytes([len(data)]))
    return b'\x0d\x01' + fixed + data


class Radio:
    def __init__(self, peer=None):
        self.peer = peer
        self.calls = []
        self.host = False
        self.option = 0
        self.data = b''
        self.fail_opcode = None

    def command(self, opcode, payload=b'', allow_fail=False):
        self.calls.append((opcode, payload))
        if opcode == self.fail_opcode:
            return 0x0C, b''
        if opcode == c.OP_SET_HOST_FEATURE:
            self.host = bool(payload[1])
        if opcode == c.OP_EXT_ADV_PARAMS_V2:
            self.option = payload[-1]
        if opcode == c.p.OP_LE_SET_EXT_ADV_DATA:
            self.data = payload[4:]
        return 0, b''

    def read_packet(self, timeout):
        phy = 1 if self.peer.option == 0 else 3
        if self.peer.option == 3 and self.host:
            phy = 4
        return c.p.H4_EVENT, c.p.EVT_LE_META, report(self.peer.data, phy, phy)


class Core54Test(unittest.TestCase):
    def test_profile_requires_advertised_54_features_and_commands(self):
        caps = {
            'version': bytes([0x0D, 0, 0, 0x0D, 0, 0, 0, 0]),
            'commands': bytes(64),
            'features': bytes(8),
        }
        errors = c.profile54_errors(caps)
        for name in ('Advertising Coding Selection', 'PAwR Advertiser',
                     'PAwR Scanner', 'LE Set Host Feature',
                     'LE Set Extended Advertising Parameters v2',
                     'LE Set Periodic Advertising Subevent Data',
                     'LE Set Periodic Advertising Response Data',
                     'LE Set Periodic Sync Subevent',
                     'LE Extended Create Connection v2',
                     'LE Set Periodic Advertising Parameters v2'):
            self.assertTrue(any(name in err for err in errors), name)

    def test_profile_accepts_required_54_advertisement(self):
        commands = bytearray(64)
        features = bytearray(8)
        for bit, _ in c.REQUIRED_FEATURES:
            features[bit // 8] |= 1 << (bit % 8)
        for octet, bit, _, _ in c.REQUIRED_COMMANDS:
            commands[octet] |= 1 << bit
        caps = {
            'version': bytes([0x10, 0, 0, 0x10, 0, 0, 0, 0]),
            'commands': bytes(commands),
            'features': bytes(features),
        }
        self.assertEqual(c.profile54_errors(caps), [])

    def test_profile_rejects_pre_54_version(self):
        commands = bytearray(64)
        features = bytearray(8)
        for bit, _ in c.REQUIRED_FEATURES:
            features[bit // 8] |= 1 << (bit % 8)
        for octet, bit, _, _ in c.REQUIRED_COMMANDS:
            commands[octet] |= 1 << bit
        caps = {
            'version': bytes([0x0C, 0, 0, 0x0C, 0, 0, 0, 0]),
            'commands': bytes(commands),
            'features': bytes(features),
        }
        self.assertTrue(any('below 5.4' in err
                            for err in c.profile54_errors(caps)))

    def test_parameter_encodings(self):
        for option in (0, 3, 4):
            data = c.advertising_params(1, option)
            self.assertEqual(len(data), 27)
            self.assertEqual(data[20], 3 if option else 1)
            self.assertEqual(data[22], data[20])
            self.assertEqual(data[-2:], bytes([option, option]))

    def test_parser_bounds_and_multiple_reports(self):
        one = report(b'abc')
        self.assertEqual(c.reports(one)[0][-1], b'abc')
        self.assertEqual(len(c.reports(b'\x0d\x02' + one[2:] * 2)), 2)
        for bad in (b'\x0d', one[:-1], one + b'\x00', b'\x0d\x02' + one[2:]):
            with self.assertRaises(c.p.HciError):
                c.reports(bad)
        self.assertEqual(c.reports(b'\x0e\x00'), [])

    def test_wrong_phy_and_incomplete_data_fail(self):
        scanner = Radio()
        for body in (report(b'x', primary=3), report(b'x', flags=0x20),
                     report(b'x', flags=0x40)):
            with patch.object(scanner, 'read_packet', return_value=(
                    c.p.H4_EVENT, c.p.EVT_LE_META, body)):
                with self.assertRaises(c.p.HciError):
                    c.observe(scanner, ADDR, 1, b'x', 4)

    def test_unrelated_peer_and_stale_data_are_ignored(self):
        scanner = Radio()
        packets = [report(b'x', addr=bytes(6)), report(b'old'), report(b'x')]
        with patch.object(scanner, 'read_packet', side_effect=[
                (c.p.H4_EVENT, c.p.EVT_LE_META, body) for body in packets]) as read:
            c.observe(scanner, ADDR, 1, b'x', 4)
            self.assertEqual(read.call_count, 3)

    def run_phase(self, fail=None):
        advertiser = Radio()
        scanner = Radio(advertiser)
        advertiser.fail_opcode = fail
        book = ResultBook('test')
        with patch.object(c, 'prepare_controller', return_value=(ADDR, 1, 'test')):
            c.run_advertising54_phase(book, 'pair', advertiser, scanner)
        return book, advertiser, scanner

    def test_all_cases_and_ead_transport(self):
        book, advertiser, scanner = self.run_phase()
        self.assertEqual(book.exit_code(), 0)
        self.assertEqual(len(book.results), 4)
        self.assertIn(b'\x1e\x31' + c.EAD_SAMPLE, advertiser.data)
        self.assertEqual([v for op, v in scanner.calls if op == c.OP_SET_HOST_FEATURE],
                         [b'\x29\x01'] * 3 + [b'\x29\x00'])
        for radio in (advertiser, scanner):
            self.assertEqual(sum(op == 0x0C03 for op, _ in radio.calls), 4)

    def test_rejected_command_and_cleanup_never_pass(self):
        for opcode in (c.OP_EXT_ADV_PARAMS_V2, 0x0C03):
            book, advertiser, scanner = self.run_phase(opcode)
            self.assertEqual(book.exit_code(), 1)
            self.assertEqual(sum(op == 0x0C03 for op, _ in scanner.calls), 4)

    def test_both_roles(self):
        import release_test as release
        with patch.object(release, 'with_pair') as run:
            release.run_54_phases('book', 'A', 'B', False)
            self.assertEqual(run.call_count, 5)
            self.assertEqual([call.args[:2] for call in run.call_args_list],
                             [('A', 'B'), ('A', 'B'), ('A', 'B'),
                              ('B', 'A'), ('B', 'A')])
            self.assertIs(run.call_args_list[0].args[3],
                          release.run_profile54_phase)


if __name__ == '__main__':
    unittest.main()

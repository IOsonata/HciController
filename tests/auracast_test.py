#!/usr/bin/env python3
"""Auracast source: byte layout of the broadcast data and the HCI sequence.

The layouts are checked against the BAP 1.0 and PBP 1.0 field order written
out by hand below, not against a second implementation. The final check is an
Auracast receiver decoding the stream on air.
"""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from hcicontroller import auracast as ac  # noqa: E402


class LayoutTest(unittest.TestCase):
    def test_codec_config_48k_10ms_100_octets(self):
        self.assertEqual(
            ac.codec_config(48000, 10000, 100),
            bytes.fromhex("020108" "020201" "03046400"))

    def test_channel_allocation(self):
        self.assertEqual(ac.channel_allocation(ac.LOCATION_FRONT_RIGHT),
                         bytes.fromhex("0503" "02000000"))

    def test_metadata_media_context(self):
        self.assertEqual(ac.metadata(ac.CONTEXT_MEDIA),
                         bytes.fromhex("030204" "00"))

    def test_stereo_base(self):
        got = ac.stereo_base(40000, 48000, 10000, 100)
        want = bytes.fromhex(
            "40" "9C00"                     # presentation delay 40000 us
            "01"                            # one subgroup
            "02"                            # two BIS
            "0600000000"                    # LC3
            "0A" "020108" "020201" "03046400"
            "04" "03020400"                 # metadata: media
            "01" "06" "0503" "01000000"     # BIS 1 left
            "02" "06" "0503" "02000000")    # BIS 2 right
        body = bytes([len(want) + 3, 0x16, 0x51, 0x18]) + want
        self.assertEqual(got, body)

    def test_announcements(self):
        self.assertEqual(ac.broadcast_announcement(0x1A2B3C),
                         bytes.fromhex("0616" "5218" "3C2B1A"))
        self.assertEqual(ac.public_announcement(ac.PBA_STANDARD_QUALITY),
                         bytes.fromhex("0516" "5618" "02" "00"))
        self.assertEqual(ac.broadcast_name("Test"),
                         bytes.fromhex("0530") + b"Test")

    def test_extended_adv_data_encrypted(self):
        data = ac.extended_adv_data(1, "Test", encrypted=True)
        self.assertEqual(data[7:12], bytes.fromhex("0516" "5618" "03"))

    def test_name_and_code_limits(self):
        with self.assertRaises(ValueError):
            ac.broadcast_name("abc")
        with self.assertRaises(ValueError):
            ac.broadcast_code("x" * 17)
        self.assertEqual(ac.broadcast_code("1234"), b"1234" + bytes(12))

    def test_periodic_data_fragments(self):
        one = ac.periodic_data_commands(0, bytes(100))
        self.assertEqual(len(one), 1)
        self.assertEqual(one[0][:3], bytes([0, 3, 100]))
        three = ac.periodic_data_commands(0, bytes(600))
        self.assertEqual([c[1] for c in three], [0, 1, 2])
        self.assertEqual(sum(c[2] for c in three), 600)

    def test_create_big_payload(self):
        p = ac.create_big_payload(2, 10000, 100, 4, 65, 2)
        self.assertEqual(len(p), 31)
        self.assertEqual(p[:3], bytes([0, 0, 2]))
        self.assertEqual(p[3:6], bytes.fromhex("102700"))
        self.assertEqual(struct.unpack("<HH", p[6:10]), (100, 65))
        self.assertEqual(p[10:15], bytes([4, 2, 0, 0, 0]))
        enc = ac.create_big_payload(2, 10000, 100, 4, 65, 2, ac.broadcast_code("pw"))
        self.assertEqual(enc[14], 1)
        self.assertEqual(enc[15:17], b"pw")


class FakeHci:
    """Controller that accepts everything and reports a two BIS BIG."""

    def __init__(self):
        self.sent = []
        self.iso = []
        self.events = []

    def setup(self):
        pass

    def identity(self, override=None):
        return bytes(range(1, 7)), 0x01, "fake"

    def command(self, opcode, payload=b"", allow_fail=False):
        self.sent.append((opcode, bytes(payload)))
        if opcode == ac.OP_LE_CREATE_BIG:
            ev = bytearray(21 + 2)
            ev[0] = ac.LE_BIG_COMPLETE
            ev[2] = 0
            ev[18] = 2
            ev[19:23] = struct.pack("<HH", 0x0100, 0x0101)
            self.events.append((4, 0x3E, bytes(ev)))
        return 0, b""

    def read_packet(self, timeout=0.0):
        if self.events:
            return self.events.pop(0)
        return None

    def send_iso(self, handle, seq, sdu):
        self.iso.append((handle, seq, bytes(sdu)))


class SequenceTest(unittest.TestCase):
    def test_start_stream_stop(self):
        hci = FakeHci()
        src = ac.AuracastSource(hci, 0x123456, "Test Cast", password="pw")
        src.start()
        self.assertEqual(src.bis_handles, [0x0100, 0x0101])
        ops = [o for o, _ in hci.sent]
        # Advertising set and the train come before the BIG, and the data
        # paths come after it.
        big = ops.index(ac.OP_LE_CREATE_BIG)
        self.assertLess(ops.index(0x2036), ops.index(ac.OP_LE_SET_PERIODIC_ADV_DATA))
        self.assertLess(ops.index(ac.OP_LE_SET_PERIODIC_ADV_DATA), big)
        self.assertLess(ops.index(0x2040), big)
        self.assertEqual(ops.count(0x206E), 2)
        self.assertGreater(ops.index(0x206E), big)

        count = [0]

        def frames():
            count[0] += 1
            return None if count[0] > 3 else [b"L" * 100, b"R" * 100]

        src.stream(frames)
        self.assertEqual(len(hci.iso), 6)
        self.assertEqual([h for h, _, _ in hci.iso[:2]], [0x0100, 0x0101])
        self.assertEqual([q for _, q, _ in hci.iso[::2]], [0, 1, 2])

        src.stop()
        self.assertEqual(hci.sent[-3][0], ac.OP_LE_TERMINATE_BIG)
        self.assertEqual(src.bis_handles, [])


if __name__ == "__main__":
    unittest.main()

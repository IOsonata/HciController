#!/usr/bin/env python3
"""Auracast source on the PC.

Opens one HciController, encodes LC3 on the PC with liblc3 (pip install lc3py)
and broadcasts a stereo stream: a tone per channel, or a WAV file.

    python3 auracast_source.py --port /dev/ttyACM0 --name "IOsonata Test"
    python3 auracast_source.py --usb 1915:521f --wav music.wav --seconds 60

The WAV file has to be 16 bit and match the preset sampling rate.
"""

import argparse
import array
import math
import sys
import wave

from hcicontroller import Hci, SelectionError, TransportSpec, discover
from hcicontroller import auracast
from hci_cis_pair_test import IsoHci


class Tone:
    def __init__(self, rate, left_hz, right_hz):
        self.rate = rate
        self.freq = (left_hz, right_hz)
        self.pos = 0

    def pcm(self, count):
        out = []
        for hz in self.freq:
            out.append(array.array("h", [
                int(9000 * math.sin(2 * math.pi * hz * (self.pos + i) / self.rate))
                for i in range(count)]))
        self.pos += count
        return out


class WavFile:
    def __init__(self, path, rate):
        with wave.open(path, "rb") as w:
            if w.getsampwidth() != 2:
                raise SystemExit("WAV file must be 16 bit")
            if w.getframerate() != rate:
                raise SystemExit("WAV file must be %d Hz" % rate)
            self.channels = w.getnchannels()
            self.data = array.array("h")
            self.data.frombytes(w.readframes(w.getnframes()))
        self.pos = 0

    def pcm(self, count):
        out = [array.array("h"), array.array("h")]
        n = len(self.data) // self.channels
        for i in range(count):
            at = (self.pos + i) % n
            left = self.data[at * self.channels]
            right = self.data[at * self.channels + (1 if self.channels > 1 else 0)]
            out[0].append(left)
            out[1].append(right)
        self.pos = (self.pos + count) % n
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial H:4 device")
    ap.add_argument("--usb", metavar="SERIAL_OR_VID:PID")
    ap.add_argument("--name", default="IOsonata Auracast")
    ap.add_argument("--id", type=lambda v: int(v, 0), default=0x1A2B3C,
                    help="24 bit Broadcast_ID")
    ap.add_argument("--preset", default="48_2_2", choices=sorted(auracast.PRESETS))
    ap.add_argument("--password", help="Broadcast_Code, turns encryption on")
    ap.add_argument("--wav")
    ap.add_argument("--tone", nargs=2, type=float, default=(440.0, 660.0),
                    metavar=("LEFT_HZ", "RIGHT_HZ"))
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--phy", type=int, choices=(1, 2), default=2)
    ap.add_argument("--rtn", type=int, default=4)
    ap.add_argument("--latency", type=int, default=65, help="ms")
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args()

    try:
        import lc3
    except ImportError:
        print("FAIL: pip install lc3py", file=sys.stderr)
        return 2

    try:
        if args.port:
            spec = TransportSpec("serial", args.port, "serial H:4 %s" % args.port)
        else:
            spec = discover(kind="usb" if args.usb else "auto", usb_selector=args.usb)
            if spec is None:
                raise SelectionError("no HciController found")
    except SelectionError as err:
        print("FAIL: %s" % err, file=sys.stderr)
        return 2

    rate, frame_us, octets = auracast.PRESETS[args.preset]
    samples = rate * frame_us // 1000000
    encoders = [lc3.Encoder(frame_us, rate), lc3.Encoder(frame_us, rate)]
    audio = WavFile(args.wav, rate) if args.wav else Tone(rate, *args.tone)

    def frames():
        left, right = audio.pcm(samples)
        return [enc.encode(pcm.tobytes(), octets, bit_depth=16)
                for enc, pcm in zip(encoders, (left, right))]

    hci = IsoHci(spec, raw=args.raw)
    src = auracast.AuracastSource(
        hci, args.id, args.name, preset=args.preset, rtn=args.rtn,
        latency_ms=args.latency, phy=args.phy, password=args.password)
    try:
        src.start()
        print("Broadcasting '%s' id 0x%06X, BIS handles %s. Ctrl-C to stop."
              % (args.name, args.id, src.bis_handles))
        src.stream(frames, args.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        src.stop()
        hci.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

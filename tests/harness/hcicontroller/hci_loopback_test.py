#!/usr/bin/env python3
"""Transport-neutral HCI PRBS loopback integrity and timing test."""

import argparse
from pathlib import Path
import struct
import sys
import time
import zlib

_HARNESS_DIR = Path(__file__).resolve().parents[1]
_LIB_DIR = _HARNESS_DIR / "lib"
if str(_LIB_DIR) not in sys.path:
    sys.path.insert(0, str(_LIB_DIR))

import hci_ble_test
import hci_transport


OP_VS_TRANSPORT_LOOPBACK = 0xFFF1
REQUEST_HEADER_LEN = 3
RETURN_HEADER_LEN = 10
MAX_DATA_LEN = 240
RX_PRBS_ERROR = 0x01
RX_BAD_SEED = 0x02
NO_ERROR_INDEX = 0xFFFF

# HCI packet bytes, excluding an H:4 packet indicator and lower transport
# framing. The command is Opcode(2) + Parameter_Length(1) + the loopback
# request header. Command Complete is Event_Code(1) + Parameter_Length(1) +
# Num_HCI_Command_Packets(1) + Opcode(2) + Status(1) + loopback return header.
REQUEST_HCI_BASE_LEN = 3 + REQUEST_HEADER_LEN
RETURN_HCI_BASE_LEN = 6 + RETURN_HEADER_LEN
HCI_BASE_BYTES_PER_CASE = REQUEST_HCI_BASE_LEN + RETURN_HCI_BASE_LEN
H4_INDICATOR_BYTES_PER_CASE = 2


def prbs8(seed, length):
    """x^8 + x^6 + x^5 + x^4 + 1; current state is the next octet."""
    state = seed & 0xFF
    out = bytearray()
    for _ in range(length):
        value = state
        out.append(value)
        feedback = ((value >> 7) ^ (value >> 5) ^
                    (value >> 4) ^ (value >> 3)) & 1
        state = ((value << 1) & 0xFF) | feedback
    return bytes(out)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def first_difference(expected, actual):
    limit = min(len(expected), len(actual))
    for index in range(limit):
        if expected[index] != actual[index]:
            return index
    if len(expected) != len(actual):
        return limit
    return None


def resolve_transport(args):
    if args.port is not None:
        if args.transport == "usb" or args.usb is not None:
            raise hci_transport.SelectionError(
                "--port selects serial H:4 and cannot be combined with USB")
        return hci_transport.TransportSpec(
            "serial", args.port, "serial H:4 %s" % args.port)

    if args.usb is not None:
        if args.transport == "serial":
            raise hci_transport.SelectionError(
                "--usb cannot be combined with --transport serial")
        return hci_transport.discover("usb", usb_selector=args.usb)

    return hci_transport.discover(args.transport)


def decode_return(data, expected_size):
    if len(data) != RETURN_HEADER_LEN + expected_size:
        raise hci_ble_test.HciError(
            "loopback return length %u, expected %u" %
            (len(data), RETURN_HEADER_LEN + expected_size)
        )

    sequence, = struct.unpack("<H", data[0:2])
    seed = data[2]
    flags = data[3]
    first_bad, = struct.unpack("<H", data[4:6])
    received_crc, = struct.unpack("<I", data[6:10])
    return sequence, seed, flags, first_bad, received_crc, data[10:]


def run_case(hci, sequence, size, inject_error=False):
    seed = ((sequence * 73 + size * 29 + 1) & 0xFF) or 0xA5
    expected_prbs = prbs8(seed, size)
    transmitted = bytearray(expected_prbs)
    injected_index = None

    if inject_error and size:
        injected_index = size // 2
        transmitted[injected_index] ^= 0x01

    transmitted = bytes(transmitted)
    request = struct.pack("<HB", sequence, seed) + transmitted

    try:
        status, data = hci.command(
            OP_VS_TRANSPORT_LOOPBACK, request, timeout=3.0, allow_fail=True)
    except (hci_ble_test.HciError, hci_ble_test.HciGone) as err:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: %s" % (sequence, size, err))

    if status != 0:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: command status 0x%02X" %
            (sequence, size, status))

    (rx_sequence, rx_seed, flags, first_bad,
     received_crc, echo) = decode_return(data, size)

    if rx_sequence != sequence or rx_seed != seed:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: returned header changed: seq=%u seed=0x%02X "
            "expected seq=%u seed=0x%02X" %
            (sequence, size, rx_sequence, rx_seed, sequence, seed))

    if flags & RX_BAD_SEED:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: controller received an invalid zero PRBS seed" %
            (sequence, size))

    tx_crc = crc32(transmitted)
    echo_crc = crc32(echo)

    if inject_error:
        if size == 0:
            return
        if not (flags & RX_PRBS_ERROR):
            raise hci_ble_test.HciError(
                "seq=%u size=%u: controller did not report injected RX error" %
                (sequence, size))
        if first_bad != injected_index:
            raise hci_ble_test.HciError(
                "seq=%u size=%u: injected error index %u, controller reported %u" %
                (sequence, size, injected_index, first_bad))
        if received_crc != tx_crc or echo != transmitted:
            raise hci_ble_test.HciError(
                "seq=%u size=%u: error report returned different received bytes" %
                (sequence, size))
        return

    if flags & RX_PRBS_ERROR:
        difference = first_difference(expected_prbs, echo)
        raise hci_ble_test.HciError(
            "seq=%u size=%u: HOST->CONTROLLER PRBS error; controller first_bad=%u "
            "echo_first_bad=%s rx_crc=0x%08X expected_crc=0x%08X" %
            (sequence, size, first_bad,
             "none" if difference is None else str(difference),
             received_crc, crc32(expected_prbs)))

    if first_bad != NO_ERROR_INDEX:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: clean RX flag with first_bad=%u" %
            (sequence, size, first_bad))

    if received_crc != tx_crc:
        raise hci_ble_test.HciError(
            "seq=%u size=%u: returned RX CRC changed: 0x%08X expected 0x%08X" %
            (sequence, size, received_crc, tx_crc))

    if echo != transmitted:
        difference = first_difference(transmitted, echo)
        raise hci_ble_test.HciError(
            "seq=%u size=%u: CONTROLLER->HOST echo error at byte %s; "
            "controller RX was clean, controller_crc=0x%08X host_echo_crc=0x%08X" %
            (sequence, size,
             "unknown" if difference is None else str(difference),
             received_crc, echo_crc))


def rate_kbytes(byte_count, elapsed):
    return (byte_count / elapsed) / 1000.0 if elapsed > 0.0 else 0.0


def format_timing(elapsed, packets, payload_one_way, hci_bytes, serial_h4):
    if elapsed <= 0.0 or packets <= 0:
        return "elapsed=0"

    packet_rate = packets / elapsed
    average_ms = (elapsed * 1000.0) / packets
    payload_round_trip = payload_one_way * 2

    text = (
        "elapsed=%.3fs  %.1f pkt/s  %.3f ms/txn  "
        "PRBS=%.1f kB/s/dir %.1f kB/s round-trip  HCI=%.1f kB/s"
        % (elapsed, packet_rate, average_ms,
           rate_kbytes(payload_one_way, elapsed),
           rate_kbytes(payload_round_trip, elapsed),
           rate_kbytes(hci_bytes, elapsed))
    )

    if serial_h4:
        h4_bytes = hci_bytes + packets * H4_INDICATOR_BYTES_PER_CASE
        text += "  H4=%.1f kB/s" % rate_kbytes(h4_bytes, elapsed)

    return text


def main():
    parser = argparse.ArgumentParser(
        description=(
            "PRBS HCI command loopback over serial H:4 or native Bluetooth USB"
        )
    )
    parser.add_argument(
        "--transport", choices=("auto", "serial", "usb"), default="auto")
    parser.add_argument("--port", help="UART/CDC H:4 port")
    parser.add_argument("--usb", help="native USB serial number or VID:PID")
    parser.add_argument("--min-size", type=int, default=0)
    parser.add_argument("--max-size", type=int, default=MAX_DATA_LEN)
    parser.add_argument("--step", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=1,
                        help="number of complete size sweeps")
    parser.add_argument("--verify-error-report", action="store_true",
                        help="first prove the controller detects a deliberately bad PRBS byte")
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()

    if args.min_size < 0 or args.max_size > MAX_DATA_LEN or \
            args.min_size > args.max_size:
        parser.error("sizes must satisfy 0 <= min <= max <= %u" % MAX_DATA_LEN)
    if args.step <= 0:
        parser.error("--step must be positive")
    if args.repeat <= 0:
        parser.error("--repeat must be positive")

    try:
        spec = resolve_transport(args)
    except hci_transport.SelectionError as err:
        print("FAIL:", err, file=sys.stderr)
        return 2

    if spec is None:
        print("FAIL: no HciController transport found", file=sys.stderr)
        return 2

    print("Transport:", spec)
    print("sizes=%u..%u step=%u repeat=%u" %
          (args.min_size, args.max_size, args.step, args.repeat))

    serial_h4 = spec.kind == "serial"
    hci = None
    sequence = 0
    passed = 0
    payload_one_way = 0
    hci_bytes = 0
    run_start = None
    run_elapsed = 0.0

    try:
        hci = hci_ble_test.Hci(spec, raw=args.raw)

        if args.verify_error_report:
            check_size = max(1, min(64, args.max_size))
            verify_start = time.perf_counter()
            run_case(hci, sequence, check_size, inject_error=True)
            verify_elapsed = time.perf_counter() - verify_start
            print("Controller RX error reporting: PASS (%.3f ms)" %
                  (verify_elapsed * 1000.0))
            sequence = (sequence + 1) & 0xFFFF

        run_start = time.perf_counter()

        for sweep in range(args.repeat):
            print("[sweep %u/%u]" % (sweep + 1, args.repeat))
            sweep_start = time.perf_counter()
            sweep_packets = 0
            sweep_payload_one_way = 0
            sweep_hci_bytes = 0

            for size in range(args.min_size, args.max_size + 1, args.step):
                run_case(hci, sequence, size)
                passed += 1
                sweep_packets += 1
                payload_one_way += size
                sweep_payload_one_way += size
                case_hci_bytes = HCI_BASE_BYTES_PER_CASE + (2 * size)
                hci_bytes += case_hci_bytes
                sweep_hci_bytes += case_hci_bytes
                sequence = (sequence + 1) & 0xFFFF

            sweep_elapsed = time.perf_counter() - sweep_start
            print("   PASS through size %u (%u total packets)" %
                  (args.max_size, passed))
            print("   " + format_timing(
                sweep_elapsed, sweep_packets, sweep_payload_one_way,
                sweep_hci_bytes, serial_h4))

        run_elapsed = time.perf_counter() - run_start

    except (hci_ble_test.HciError, hci_ble_test.HciGone) as err:
        elapsed = (time.perf_counter() - run_start
                   if run_start is not None else 0.0)
        print("FAIL:", err)
        print("passed=%u" % passed)
        if passed:
            print("Timing before failure:")
            print("   " + format_timing(
                elapsed, passed, payload_one_way, hci_bytes, serial_h4))
        return 1
    finally:
        if hci is not None:
            hci.close()

    elapsed = run_elapsed
    print("PASS: %u loopback packets" % passed)
    print("Timing:")
    print("   " + format_timing(
        elapsed, passed, payload_one_way, hci_bytes, serial_h4))
    print("   PRBS bytes: %u each direction, %u round-trip" %
          (payload_one_way, payload_one_way * 2))
    print("   HCI bytes: %u%s" %
          (hci_bytes,
           " (USB framing excluded)" if not serial_h4 else ""))
    if serial_h4:
        h4_bytes = hci_bytes + passed * H4_INDICATOR_BYTES_PER_CASE
        print("   H:4 wire bytes: %u" % h4_bytes)
    return 0


if __name__ == "__main__":
    sys.exit(main())

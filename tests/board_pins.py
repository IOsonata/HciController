#!/usr/bin/env python3
"""
The board pin map, checked against itself and against the README.

nRF52840/src/board.h is compiled by the target build and by nothing else.
The host tests build against tests/stubs/board.h, so every pin number, rate
and flow control setting in the real file has only ever been checked by an
arm-none-eabi build that happened to be for that one board.

That is how the dongle came to hold P0.25, P1.00, P0.19 and P0.22 as its
UART: they are the Nordic Thingy:91 nRF52840 interconnect, copied in and left,
with RTS and CTS crossed on the way. Nothing compiled it, nothing compared it
with the README, which says the BLYST840 product wires TXD P0.24 and RXD
P0.23, and the two disagreed for as long as both existed.

What this checks, per board:

    the four UART pins are four different pins
    a board asking for flow control names all four
    the rate is stated

and across files:

    the pin numbers the README quotes are the ones the named board has

None of that would have caught the crossed RTS and CTS, which needed a
schematic. All of it would have caught two boards sharing one pin map.
"""

import os
import re
import sys


def read_boards(path):
    """
    Split board.h into its per board branches.

    A textual walk of the #if chain rather than a preprocessor, because the
    file is a chain of plain equality tests on BOARD and the point is to see
    every branch at once, which a preprocessor cannot do.
    """
    text = open(path).read()
    boards = {}
    current = None
    depth = 0
    for line in text.split("\n"):
        stripped = line.strip()
        match = re.match(r"#(?:el)?if\s+BOARD\s*==\s*(\w+)", stripped)
        if match:
            current = match.group(1)
            boards[current] = []
            depth = 0
            continue
        if current is None:
            continue
        # Only the branch's own top level. A nested #ifndef HCI_HOST_SELECT
        # must not end it.
        if re.match(r"#if", stripped):
            depth += 1
        elif re.match(r"#endif", stripped):
            if depth == 0:
                current = None
                continue
            depth -= 1
        elif re.match(r"#el(?:se|if)", stripped) and depth == 0:
            current = None
            continue
        if current is not None:
            boards[current].append(line)
    return {name: "\n".join(body) for name, body in boards.items()}


def define(body, name):
    match = re.search(r"^#define\s+%s\s+(\S+)" % re.escape(name), body,
                      re.MULTILINE)
    return match.group(1) if match else None


def pin(body, signal):
    port = define(body, "UART_%s_PORT" % signal)
    number = define(body, "UART_%s_PIN" % signal)
    if port is None or number is None:
        return None
    return (int(port, 0), int(number, 0))


def pin_str(value):
    return "P%d.%02d" % value


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    header = os.path.join(root, "nRF52840", "src", "board.h")
    readme = os.path.join(root, "README.md")
    if not os.path.exists(header):
        print("[TEST] board_pins.py skipped, no %s" % header)
        return 0

    boards = read_boards(header)
    if not boards:
        print("[!!] no board branches found in board.h")
        return 1

    bad = 0
    for name in sorted(boards):
        body = boards[name]
        pins = {s: pin(body, s) for s in ("TX", "RX", "RTS", "CTS")}
        named = {s: v for s, v in pins.items() if v is not None}

        if pins["TX"] is None or pins["RX"] is None:
            print("[!!] %s names no TX or no RX" % name)
            bad += 1
            continue

        seen = {}
        for signal, value in named.items():
            if value in seen:
                print("[!!] %s puts %s and %s both on %s"
                      % (name, seen[value], signal, pin_str(value)))
                bad += 1
            seen[value] = signal

        flow = define(body, "UART_HW_FLOWCTRL")
        if flow is not None and flow.rstrip("Uu") == "1":
            missing = [s for s in ("RTS", "CTS") if pins[s] is None]
            if missing:
                print("[!!] %s asks for flow control and names no %s"
                      % (name, " or ".join(missing)))
                bad += 1

        rate = define(body, "UART_RATE")
        if rate is None or int(rate, 0) == 0:
            print("[!!] %s states no UART rate" % name)
            bad += 1

        if not bad:
            flow_text = "flow control" if flow and \
                flow.rstrip("Uu") == "1" else "no flow control"
            print("[ok] %-22s %s tx, %s rx, %s, %s bit/s"
                  % (name, pin_str(pins["TX"]), pin_str(pins["RX"]),
                     flow_text, rate))

    # The README quotes pin numbers for the BLYST840 product, which is the
    # module the dongle is built on. Those two disagreed for the whole life of
    # both files and nothing said so.
    if os.path.exists(readme):
        text = open(readme).read()
        quoted = dict(re.findall(r"BLYST840 (TXD|RXD):\s*P(\d\.\d+)", text))
        wanted = {"TXD": "TX", "RXD": "RX"}
        body = boards.get("UDG_NRF52840")
        for label, signal in wanted.items():
            if label not in quoted or body is None:
                continue
            port, number = quoted[label].split(".")
            want = (int(port), int(number))
            have = pin(body, signal)
            if have != want:
                print("[!!] README says BLYST840 %s is P%s and board.h "
                      "UDG_NRF52840 has %s"
                      % (label, quoted[label], pin_str(have)))
                bad += 1
        if not bad:
            print("[ok] README pin numbers agree with UDG_NRF52840")

    if bad:
        print()
        print("%d board pin problem(s)." % bad)
        return 1
    print("[ok] %d board(s) checked." % len(boards))
    return 0


if __name__ == "__main__":
    sys.exit(main())

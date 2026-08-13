#!/usr/bin/env python3
"""Guard the USB IRQ storm pass boundary in hci_app.cpp.

The nRF52840 storm detector measures USBD IRQs since UsbPassMark.  A missing
steady-state mark in 70088f5 made ordinary sustained traffic eventually look
like a storm and deliberately disconnected USB after the cumulative IRQ count
crossed the limit.  The nRF unit test checks the detector itself; this check
pins the application-side call ordering that defines each measurement window.
"""

from pathlib import Path
import sys


def die(message: str) -> None:
    print(f"[FAIL] {message}")
    raise SystemExit(1)


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        die(f"cannot find {signature}")

    brace = source.find("{", start)
    if brace < 0:
        die(f"cannot find body for {signature}")

    depth = 0
    state = "code"
    i = brace
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""

        if state == "line_comment":
            if ch == "\n":
                state = "code"
        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "code"
        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "code"
        else:
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[brace + 1:i]
        i += 1

    die(f"unterminated body for {signature}")
    return ""


def require_usb_pass_order(body: str, label: str) -> None:
    names = (
        "UsbPowerProcess",
        "HciTargetUsbStuck",
        "UsbPassMark",
        "HciTinyUsbProcess",
    )
    positions = [body.find(name) for name in names]

    missing = [name for name, pos in zip(names, positions) if pos < 0]
    if missing:
        die(f"{label}: missing {', '.join(missing)}")

    if positions != sorted(positions):
        die(
            f"{label}: expected UsbPowerProcess -> UsbStuck -> "
            "UsbPassMark -> HciTinyUsbProcess"
        )


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: usb_runtime_pass.py /path/to/HciController")

    root = Path(sys.argv[1]).resolve()
    source = (root / "src" / "hci_app.cpp").read_text(encoding="utf-8")

    start = function_body(
        source, "static bool HciAppHostStart(void *pContext)"
    )
    settle_at = start.find("for (uint32_t pass")
    if settle_at < 0:
        die("HciAppHostStart: cannot find USB settle loop")
    require_usb_pass_order(start[settle_at:], "USB enumeration")

    runtime = function_body(
        source, "static void HciAppHostProcess(void *pContext)"
    )
    require_usb_pass_order(runtime, "steady-state USB")

    print("[ok] USB IRQ storm window is reset before each TinyUSB service pass")


if __name__ == "__main__":
    main()

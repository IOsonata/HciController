#!/usr/bin/env python3
"""Guard IOsonata USB lifecycle and processing in hci_app.cpp."""

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
    process_at = start.find("UsbProcess(0)", settle_at)
    configured_at = start.find("UsbConfigured(0)", process_at)
    if process_at < 0 or configured_at < 0 or process_at > configured_at:
        die("USB enumeration must process IOsonata before testing configured")

    runtime = function_body(
        source, "static void HciAppHostProcess(void *pContext)"
    )
    if "UsbProcess(0)" not in runtime:
        die("steady-state USB must pump IOsonata")

    stop = function_body(source, "void HciAppStop(HciApp_t *pApp)")
    release_at = stop.find("HciAppUsbRelease(pApp)")
    target_at = stop.find("pApp->Target.pOps->Stop")
    if release_at < 0 or target_at < 0 or release_at > target_at:
        die("USB must be disabled before MPSL target teardown")

    print("[ok] IOsonata USB is pumped and disabled before target teardown")


if __name__ == "__main__":
    main()

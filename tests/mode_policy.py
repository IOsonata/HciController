#!/usr/bin/env python3
"""Check board HCI mode policy and persistent mode plumbing."""

import os
import re
import sys


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def board_branches(text):
    boards = {}
    current = None
    depth = 0
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"#(?:el)?if\s+BOARD\s*==\s*(\w+)", s)
        if m:
            current = m.group(1)
            boards[current] = []
            depth = 0
            continue
        if current is None:
            continue
        if re.match(r"#if", s):
            depth += 1
        elif re.match(r"#endif", s):
            if depth == 0:
                current = None
                continue
            depth -= 1
        elif re.match(r"#el(?:se|if)", s) and depth == 0:
            current = None
            continue
        if current is not None:
            boards[current].append(line)
    return {k: "\n".join(v) for k, v in boards.items()}


def define(body, name, default=None):
    m = re.search(r"^#define\s+%s\s+(\S+)" % re.escape(name), body,
                  re.MULTILINE)
    return m.group(1) if m else default


def check(cond, message, failures):
    if cond:
        print("[ok]", message)
    else:
        print("[!!]", message)
        failures.append(message)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    board = read(os.path.join(root, "nRF52840", "src", "board.h"))
    app_h = read(os.path.join(root, "include", "hci_app.h"))
    app_cpp = read(os.path.join(root, "src", "hci_app.cpp"))
    main_cpp = read(os.path.join(root, "src", "main.cpp"))
    boards = board_branches(board)
    failures = []

    expected = {
        "UDG_NRF52840": ("1", "HCI_HOST_SELECT_AUTO", "0", "0"),
        "IBK_NRF52840": ("1", "HCI_HOST_SELECT_AUTO", "0", "0"),
        "THINGY91_NRF52840": ("0", "HCI_HOST_SELECT_UART", "1", "1"),
        "WILDTHING51": ("0", "HCI_HOST_SELECT_UART", "1", "1"),
        "WILDTHING91": ("0", "HCI_HOST_SELECT_UART", "0", "1"),
    }

    for name, (switch, host, early, sync) in expected.items():
        body = boards.get(name, "")
        check(bool(body), "%s board branch exists" % name, failures)
        if not body:
            continue
        check(define(body, "HCI_MODE_SWITCH", "0") == switch,
              "%s switch policy" % name, failures)
        check(define(body, "HCI_HOST_SELECT") == host,
              "%s default host policy" % name, failures)
        check(define(body, "HCI_UART_EARLY_STARTUP", "0") == early,
              "%s reset-coupled early UART policy" % name, failures)
        check(define(body, "HCI_H4_STARTUP_RESET_SYNC", "0") == sync,
              "%s H:4 startup sync policy" % name, failures)

    thingy = boards.get("THINGY91_NRF52840", "")
    wild91 = boards.get("WILDTHING91", "")
    for macro in ("UART_TX_PORT", "UART_TX_PIN", "UART_RX_PORT", "UART_RX_PIN",
                  "UART_RTS_PORT", "UART_RTS_PIN", "UART_CTS_PORT", "UART_CTS_PIN",
                  "UART_RATE", "UART_HW_FLOWCTRL"):
        check(define(thingy, macro) == define(wild91, macro),
              "WildThing91 shares Thingy91 %s" % macro, failures)

    check("HCI_APP_MODE_UART_H4" in app_h and
          "HCI_APP_MODE_USB_H4" in app_h and
          "HCI_APP_MODE_USB_NATIVE" in app_h,
          "all three runtime HCI modes are declared", failures)
    check("HciAppInitMode" in app_h and "HciAppInitMode" in app_cpp,
          "runtime mode initializer is declared and implemented", failures)
    check("HCI_USB_DESCRIPTOR_CDC_H4" in app_cpp and
          "HCI_USB_DESCRIPTOR_NATIVE_HCI" in app_cpp,
          "USB H:4/native descriptors are selected at runtime", failures)

    for token in ("NvmMcuCfg", "NvmRegionAddr", "HciModeNvmLoad",
                  "HciModeNvmStore", "HciAppStop", "NVIC_SystemReset"):
        check(token in main_cpp, "persistent mode path contains %s" % token,
              failures)

    check("GPREGRET" not in main_cpp,
          "mode switch does not use bootloader retained registers", failures)
    check("HciModePendingLoad" not in main_cpp,
          "mode switch has no retained pending-mode handoff", failures)

    button_start = main_cpp.find("static void HciModeButtonProcess(void)")
    button_end = main_cpp.find("#endif /* HCI_MODE_SWITCH */", button_start)
    button_path = main_cpp[button_start:button_end] if \
        button_start >= 0 and button_end > button_start else ""

    stop = button_path.find("HciAppStop(&s_HciApp)")
    store = button_path.find("HciModeNvmStore(next)")
    reset = button_path.find("NVIC_SystemReset")
    check(stop >= 0,
          "button path stops HCI runtime before persistence", failures)
    check(store > stop,
          "button path stores mode only after HCI runtime stops", failures)
    check(reset > store,
          "button path resets only after mode is stored", failures)
    check("HciTrace" not in button_path and "HciSyslog" not in button_path,
          "high-priority button path does not write the single-writer log",
          failures)

    load = main_cpp.find("s_HciMode = HciModeNvmLoad(defaultMode, &storedMode)")
    radio_init = main_cpp.find("HciAppInitMode(&s_HciApp")
    check(load >= 0 and radio_init > load,
          "persistent mode is loaded before MPSL/SDC startup", failures)

    stack = re.search(r"^#define\s+STATUS_THREAD_STACK_SIZE\s+(\d+)U?",
                      main_cpp, re.MULTILINE)
    check(stack is not None and int(stack.group(1)) >= 2048,
          "mode/status thread has control-path stack budget", failures)

    if failures:
        print("\n%d HCI mode policy problem(s)." % len(failures))
        return 1

    print("[ok] HCI mode board policy and persistence wiring checked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

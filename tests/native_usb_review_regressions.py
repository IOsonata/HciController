#!/usr/bin/env python3
"""Pin ownership and data-path invariants of the IOsonata Bluetooth HCI USB transport."""

import os
import sys


def fail(message):
    raise SystemExit("[!!] " + message)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def function_body(text, start_marker, end_marker):
    start = text.find(start_marker)
    if start < 0:
        fail("missing %s" % start_marker)
    end = text.find(end_marker, start)
    if end < 0:
        fail("missing boundary %s" % end_marker)
    return text[start:end]


def main(argv):
    if len(argv) != 2:
        print("usage: native_usb_review_regressions.py REPO_ROOT")
        return 2

    root = os.path.abspath(argv[1])

    trace = read(os.path.join(root, "src", "hci_trace.cpp"))
    trace_body = function_body(trace, "void HciTrace(", "void HciTraceInit(")
    format_at = trace_body.find("const int len = vsnprintf(")
    guard_at = trace_body.find("if (len >= 0)", format_at)
    output_at = trace_body.find("HciTraceWrite0(line);", guard_at)
    syslog_at = trace_body.find("(void)SysLogVPrintf(&s_Log, pFormat, args);",
                                output_at)
    if format_at < 0 or guard_at < 0 or output_at < 0 or syslog_at < 0 or \
            not format_at < guard_at < output_at < syslog_at:
        fail("HciTrace must guard semihosting output before SysLog")
    print("[ok] HciTrace guards formatting failure before semihosting output")

    header = read(os.path.join(root, "include", "hci_usb.h"))
    if '#include "bluetooth/bt_hci_usb.h"' not in header:
        fail("native HCI must use IOsonata BtHciUsb")
    if "usb/usbd_hci.h" in header:
        fail("native HCI still includes the old USB-owned HCI header")
    if "class HciUsb" in header or \
            os.path.exists(os.path.join(root, "src", "hci_usb.cpp")):
        fail("HciController still contains a private native HCI transport")
    print("[ok] IOsonata BtHciUsb exclusively owns native HCI transport")

    descriptors = read(os.path.join(root, "src", "usb_descriptors.c"))
    app = read(os.path.join(root, "src", "hci_app.cpp"))
    app_header = read(os.path.join(root, "include", "hci_app.h"))
    if "HciUsbDescriptorLogCdcInstance" in header or \
            "HciUsbDescriptorLogCdcInstance" in descriptors or \
            "LogCdcInterface" in app or "LogCdcInterface" in app_header:
        fail("CDC runtime must not depend on a logical CDC instance number")
    if 'usbCfg.pProduct = "I-SYST HCI Controller";' not in app:
        fail("USB product identity changed from the released controller")
    for marker in (
            "static uint8_t s_ConfigNative[HCI_USB_NATIVE_CONFIG_MAX_LEN];",
            "memcpy(&s_ConfigNative[sizeof(header)], pHci, HciLength);",
            "HciUsbFreeEndpoint(inMask, outMask, false)",
            "HciUsbFreeEndpoint(inMask, outMask, true)",
            "bool HciUsbDescriptorSetSerialHci(const BtHciUsbSerialDesc_t *pHci)",
            "pHci->Serialized.Interface.bAlternateSetting != 1U"):
        if marker not in descriptors:
            fail("native composite descriptor is missing %s" % marker)

    for stale in (".CtrlIfNo", ".NotifyEpNo", ".DataEpNo", ".ItfNo"):
        if stale in app:
            fail("HciController still configures CDC USB topology: %s" % stale)

    if "static BtHciUsb s_HciUsb;" not in app or \
            "BtHciUsbCfg_t hciCfg = {};" not in app or \
            "hciCfg.bBulkSerialization = true;" not in app:
        fail("native application path does not instantiate IOsonata BtHciUsb with Bulk Serialization")
    native_init = app.find("s_HciUsb.Init(hciCfg)")
    make_desc = app.find("s_HciUsb.MakeSerialDesc(&hciDesc", native_init)
    bind_desc = app.find("HciUsbDescriptorSetSerialHci(&hciDesc)", make_desc)
    host_cdc_init = app.find("s_HostCdc.Init(hostCfg)")
    log_cdc_init = app.find("s_LogCdc.Init(logCfg)")
    if native_init < 0 or make_desc < 0 or bind_desc < 0 or \
            host_cdc_init < 0 or log_cdc_init < 0 or \
            not native_init < make_desc < bind_desc < log_cdc_init or \
            host_cdc_init > log_cdc_init:
        fail("host USB function must register before the diagnostic CDC")
    print("[ok] allocator-owned Bluetooth HCI topology includes serialized alt 1")

    target = read(os.path.join(root, "src", "hci_nrf52840.cpp"))
    if 'extern "C" bool UsbdXtalRequest(void)' not in target or \
            'extern "C" void UsbdXtalRelease(void)' not in target:
        fail("IOsonata USB crystal hooks are missing")
    if "NRF_USBD" in target or "USBD_IRQHandler" in target:
        fail("HciController target still owns USB controller registers or IRQ")
    print("[ok] nRF52840 target only supplies MPSL crystal ownership hooks")

    project = read(os.path.join(root, "nRF52840", "ioc", ".project"))
    cproject = read(os.path.join(root, "nRF52840", "ioc", ".cproject"))
    for stale in ("TinyUSB", "tinyusb", "dcd_nrf5x_hci", "hci_tinyusb",
                  "hci_usb_tinyusb", "hci_usb_rx"):
        if stale in project or stale in cproject:
            fail("Eclipse project still contains %s" % stale)
    if "PARENT-2-PROJECT_LOC/src/hci_usb.cpp" in project:
        fail("Eclipse project still compiles the removed private HCI transport")
    makefile = read(os.path.join(root, "tests", "GNUmakefile"))
    if "$(IOSONATA_ROOT)/src/bluetooth/bt_hci_usb.cpp" not in makefile:
        fail("host test does not compile IOsonata BtHciUsb")
    if "$(IOSONATA_ROOT)/src/usb/usbd_hci.cpp" in makefile:
        fail("host test still references the old USB-owned HCI path")
    print("[ok] target consumes the Bluetooth-owned HCI transport and contains no private USB class")

    main_cpp = read(os.path.join(root, "src", "main.cpp"))
    udg_guard = ("BOARD == UDG_NRF52840 && "
                 "HCI_HOST_SELECT == HCI_HOST_SELECT_UART")
    if udg_guard not in main_cpp:
        fail("UDG forced-UART build is not blocked while pins are placeholders")
    print("[ok] UDG forced UART stays blocked until pin map validation")

    dispatch = read(os.path.join(root, "src", "hci_cmd_dispatch.cpp"))
    handler = dispatch.find("HciCmdResult_t result = pEntry->Handler")
    response = dispatch.find("if (result.Response != pEntry->Response)", handler)
    switch = dispatch.find("switch (result.Response)", handler)
    if handler < 0 or response < 0 or switch < 0 or not handler < response < switch:
        fail("dispatcher must reject response-kind mismatch before emitting")
    print("[ok] command handlers cannot change the table response event type")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

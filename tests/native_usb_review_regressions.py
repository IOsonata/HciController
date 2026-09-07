#!/usr/bin/env python3
"""Pin ownership and data-path invariants of the IOsonata native HCI class."""

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
    usb = read(os.path.join(root, "src", "hci_usb.cpp"))
    if "class HciUsb : public UsbIntrf" not in header:
        fail("HciUsb must derive directly from UsbIntrf")
    for marker in (
            "dataCfg.EpNo = HCI_USB_BULK_EP_NO;",
            "dataCfg.TxFifoBlkSize = HCI_USB_PKT_BLKSIZE;",
            "UsbIntrf::Init(dataCfg)",
            "UsbCtrlrEpRegister(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_EVENT_EP_NO)",
            "UsbRegisterFunc(vDevNo, &cfg)"):
        if marker not in usb:
            fail("native HCI USB is missing %s" % marker)
    print("[ok] HciUsb owns class policy while UsbIntrf owns bulk packet I/O")

    xfer = function_body(usb, "void HciUsb::XferHandler(",
                         "void HciUsb::ResetHandler(")
    if "UsbIntrfXferComplete" not in xfer:
        fail("bulk transfer completion no longer delegates to UsbIntrf")

    event = function_body(usb, "bool HciUsb::SendEventPacket(",
                          "int HciUsb::SendEvent(")
    zlp = usb[usb.find("bool HciUsb::SendEventZlp("):]
    if "UsbCtrlrEpSend(vDevNo, HCI_USB_EVENT_EP_NO" not in event or \
            "UsbCtrlrEpSend(vDevNo, HCI_USB_EVENT_EP_NO, 0U)" not in zlp:
        fail("Event-IN data and terminating ZLP must use registered RAM")
    if "memcpy(vEventTxTransfer" not in event:
        fail("Event-IN must stage one endpoint packet per controller transfer")
    print("[ok] Event-IN chains registered DMA packets and its terminating ZLP")

    descriptors = read(os.path.join(root, "src", "usb_descriptors.c"))
    app = read(os.path.join(root, "src", "hci_app.cpp"))
    app_header = read(os.path.join(root, "include", "hci_app.h"))
    if "HciUsbDescriptorLogCdcInstance" in header or \
            "HciUsbDescriptorLogCdcInstance" in descriptors or \
            "LogCdcInterface" in app or "LogCdcInterface" in app_header:
        fail("CDC runtime must not depend on a logical CDC instance number")
    if 'usbCfg.pProduct = "I-SYST HCI Controller";' not in app:
        fail("USB product identity changed from the released controller")
    if "HCI_USB_CDC_FUNCTION(2U, HCI_USB_STRING_LOG, 0x84U, 0x05U, 0x85U)" \
            not in descriptors:
        fail("native diagnostic CDC descriptor is not on interfaces 2/3, EP4/5")

    for stale in (".CtrlIfNo", ".NotifyEpNo", ".DataEpNo", ".ItfNo"):
        if stale in app:
            fail("HciController still configures CDC USB topology: %s" % stale)

    native_init = app.find("s_HciUsb.Init(hciCfg)")
    host_cdc_init = app.find("s_HostCdc.Init(hostCfg)")
    log_cdc_init = app.find("s_LogCdc.Init(logCfg)")
    if native_init < 0 or host_cdc_init < 0 or log_cdc_init < 0 or \
            native_init > log_cdc_init or host_cdc_init > log_cdc_init:
        fail("host USB function must register before the diagnostic CDC")

    registration = function_body(usb, "UsbFuncCfg_t cfg = {};",
                                 "if (!UsbRegisterFunc(vDevNo, &cfg))")
    if "HCI_USB_SYNC_RESERVED_EP_NO" not in usb or \
            registration.count("HCI_USB_SYNC_RESERVED_EP_NO") != 2:
        fail("native HCI must reserve the synchronous endpoint slot")
    print("[ok] IOsonata auto allocation preserves released CDC layouts")

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
    if "PARENT-2-PROJECT_LOC/src/hci_usb.cpp" not in project:
        fail("Eclipse project does not compile native HciUsb")
    print("[ok] target project contains native HciUsb and no TinyUSB sources")

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

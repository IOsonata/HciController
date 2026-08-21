#!/usr/bin/env python3
"""Pin source invariants from the native USB full-tree review."""

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

    trace = read(os.path.join(root, "include", "hci_trace.h"))
    va_end = trace.find("va_end(args);")
    negative = trace.find("if (len < 0)", va_end)
    outputs = trace.find("HciTraceWrite0(line);", va_end)
    if va_end < 0 or negative < 0 or outputs < 0 or not va_end < negative < outputs:
        fail("HciTrace must reject negative vsnprintf before using the buffer")
    print("[ok] HciTrace rejects a formatting failure before using its buffer")

    usb = read(os.path.join(root, "src", "hci_usb_tinyusb.cpp"))
    open_body = function_body(usb,
                              "static uint16_t HciUsbDriverOpen(",
                              "static bool HciUsbOpenStoredEndpoint(")
    claim = "HciUsbOpenHciEndpoints(pUsb, RhPort, HCI_USB_HCI_ALT_LEGACY)"
    claim_at = open_body.find(claim)
    sync_at = open_body.rfind("pUsb->SyncAltPresent[Alt] = 1U;")
    configured_at = open_body.find("pUsb->Configured = true;")
    if claim_at < 0 or sync_at < 0 or configured_at < 0:
        fail("native USB open transaction markers are missing")
    if not sync_at < claim_at < configured_at:
        fail("HCI endpoints must be claimed only after the whole Bluetooth descriptor parses")
    if "usbd_edpt_open(" in open_body or "HciUsbOpenEndpoint(" in open_body:
        fail("HciUsbDriverOpen contains a direct endpoint claim before parse completion")
    print("[ok] native USB parses the complete function before claiming endpoints")

    close_helper = function_body(usb,
                                 "static void HciUsbCloseEndpoint(",
                                 "static void HciUsbCloseHciEndpoints(")
    if "usbd_edpt_close(RhPort, EpAddr);" not in close_helper:
        fail("native USB endpoint close no longer reaches TinyUSB")
    if "HciUsbPlatformEndpointClosed(RhPort, EpAddr);" not in close_helper:
        fail("native USB endpoint close no longer invokes the platform reset hook")

    nrf_port = read(os.path.join(root, "nRF52840", "src", "dcd_nrf5x_hci.c"))
    required_port = (
        '#include "device/dcd.h"',
        '#include "device/usbd_pvt.h"',
        "typedef struct",
        "HciUsbDcdState_t",
        "bool dcd_init(",
        "bool dcd_edpt_open(",
        "void dcd_edpt_close_all(",
        "bool dcd_edpt_xfer(",
        "void dcd_edpt_stall(",
        "void dcd_edpt_clear_stall(",
        "void USBD_IRQHandler(void)",
        "static uint32_t HciUsbCollectEvents(void)",
        "const uint32_t DataStatus = NRF_USBD->EPDATASTATUS;",
        "NRF_USBD->EPDATASTATUS = DataStatus;",
        "NRF_USBD->EPOUTEN &= ~TU_BIT(EpNum);",
        "pXfer->Started = false;",
        "pXfer->DataReceived = false;",
        "usbd_edpt_clear_stall(RhPort, EpAddr);",
    )
    for marker in required_port:
        if marker not in nrf_port:
            fail("nRF5x USB DCD is missing %s" % marker)

    if "#include <portable/nordic/nrf5x/dcd_nrf5x.c>" in nrf_port:
        fail("nRF5x USB DCD still includes TinyUSB's portable Nordic .c file")
    if "HciUsbTinyUsb" in nrf_port:
        fail("nRF5x USB DCD still aliases TinyUSB private DCD entry points")
    if "HciUsbCollectTinyUsbEvents" in nrf_port:
        fail("nRF5x USB DCD still has the split EPDATA-preservation dispatcher")
    for helper in ("edpt_dma_end(", "edpt_dma_start(",
                   "xact_out_dma(", "xact_in_dma(", "get_td("):
        if helper in nrf_port:
            fail("nRF5x HCI DCD still depends on TinyUSB private helper %s" % helper)

    irq_body = function_body(nrf_port,
                             "void dcd_int_handler(uint8_t RhPort)",
                             "void USBD_IRQHandler(void)")
    if "NRF_USBD->EPDATASTATUS & ~HCI_USB_BULK_OUT_STATUS" in irq_body:
        fail("nRF5x HCI IRQ still removes EPOUT2 from the shared status snapshot")
    if irq_body.count("NRF_USBD->EPDATASTATUS") != 2:
        fail("nRF5x HCI IRQ must use exactly one EPDATASTATUS snapshot and one W1C")

    target_nrf = read(os.path.join(root, "src", "hci_nrf52840.cpp"))
    if "void USBD_IRQHandler(void)" in target_nrf:
        fail("nRF52840 target still owns the USBD hardware vector")
    if "HciNrf52840UsbdPendingEvents" in target_nrf:
        fail("nRF52840 target still walks USBD event registers")
    if "tusb_int_handler(0U, true)" in target_nrf:
        fail("nRF52840 target still dispatches the USBD IRQ through TinyUSB")

    target_hooks = function_body(target_nrf,
                                 'extern "C" uint32_t HciUsbPlatformIrqEnter(void)',
                                 "void HciNrf52840UsbPassMark(")
    if "NRF_USBD" in target_hooks:
        fail("nRF52840 IRQ bookkeeping hooks still touch USBD registers")
    print("[ok] nRF5x USBD hardware interrupt handling is standalone in the DCD")

    project = read(os.path.join(root, "nRF52840", "ioc", ".project"))
    local_dcd = "PARENT-1-PROJECT_LOC/src/dcd_nrf5x_hci.c"
    external_dcd = ("PARENT-3-PROJECT_LOC/external/tinyusb/src/portable/"
                    "nordic/nrf5x/dcd_nrf5x.c")
    if local_dcd not in project or external_dcd in project:
        fail("nRF52840 target must compile the standalone HciController DCD")
    print("[ok] nRF5x HCI alt switch clears stale CBI endpoint state")

    usb_tx = read(os.path.join(root, "src", "hci_usb.cpp"))
    kick = function_body(usb_tx, "bool HciUsbKickTx(", "void HciUsbTxComplete(")
    ram_zlp = "HciUsbEdptXfer(0U, EpAddr, pUsb->TxBuffer, 0U)"
    if ram_zlp not in kick:
        fail("native USB ZLP must give the nRF EasyDMA DCD a RAM pointer")
    if "HciUsbEdptXfer(0U, EpAddr, nullptr, 0U)" in kick:
        fail("native USB ZLP still passes a null EasyDMA pointer")
    print("[ok] terminating USB ZLP keeps its EasyDMA pointer in aligned RAM")

    main_cpp = read(os.path.join(root, "src", "main.cpp"))
    udg_guard = ("BOARD == UDG_NRF52840 && "
                 "HCI_HOST_SELECT == HCI_HOST_SELECT_UART")
    if udg_guard not in main_cpp:
        fail("UDG forced-UART build is not blocked while its pins are placeholders")
    print("[ok] UDG forced UART stays blocked until the pin map is validated")

    dispatch = read(os.path.join(root, "src", "hci_cmd_dispatch.cpp"))
    handler = dispatch.find("HciCmdResult_t result = pEntry->Handler")
    response = dispatch.find("if (result.Response != pEntry->Response)", handler)
    switch = dispatch.find("switch (result.Response)", handler)
    if handler < 0 or response < 0 or switch < 0 or not handler < response < switch:
        fail("dispatcher must reject a handler response-kind mismatch before emitting it")
    print("[ok] command handlers cannot change the table-declared response event type")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

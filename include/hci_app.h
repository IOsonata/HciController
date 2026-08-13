/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_APP_H
#define HCI_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "cfifo.h"
#include "coredev/uart.h"
#include "hci_controller.h"
#include "hci_sdc_resources.h"
#include "hci_target.h"
#include "hci_sdc_nrfxlib.h"
#include "hci_syslog.h"
#include "hci_taktos.h"
#include "hci_tinyusb.h"
#include "usb/usbd_cdc_intrf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_APP_PACKET_SIZE         1024U
#define HCI_APP_COMMAND_EVENT_SIZE  260U
#define HCI_APP_CDC_INTERFACE       0U

/*
 * The second CDC function, where the log goes. Present whichever port the HCI
 * stream is on: with HCI over UART the first function is simply unused and
 * this one still reaches a terminal.
 */
#define HCI_APP_LOG_INTERFACE       1U
#define HCI_APP_FIFO_DATA_SIZE      4096U
#define HCI_APP_FIFO_MEM_SIZE       CFIFO_MEMSIZE(HCI_APP_FIFO_DATA_SIZE)

typedef enum {
    HCI_APP_HOST_UART = 0,
    HCI_APP_HOST_USB  = 1,
} HciAppHost_t;

typedef struct {
    HciController_t Controller;
    HciSdc_t Sdc;

    /* Which layers the vendor specific counter readout reports. */
    HciCounters_t Counters;

    /*
     * The log is not here. It is HciSyslogDefault, outside this structure, so
     * that the memset below does not clear what was written before this layer
     * existed and so that a start up that never reaches this layer still has
     * somewhere to have said why. This layer only drains it, on the second CDC
     * function, whenever the device stack is running. Not the HCI stream and
     * never mixed with it.
     */

    UARTDev_t Uart;
    UsbdCdcDevIntrf_t UsbIntrf;
    HciTinyUsb_t Usb;
    DevIntrf_t *pHostIntrf;
    HciAppHost_t HostType;
    bool HostOpen;

    /*
     * The USB device stack is up and worth pumping. Always so when the HCI
     * stream is on USB. Also so when the HCI stream is on the UART and the
     * board says the socket is this part's, which is how a board whose host is
     * another part on the same PCB still has somewhere to put a log.
     *
     * Cleared if the peripheral cannot be brought up, which on a board running
     * off a battery with no cable in is the ordinary case and not a fault.
     */
    bool UsbRunning;

    /*
     * Whether a terminal has the log port open. Kept so the moment it is
     * opened can be noticed, which is when the log says it is there.
     */
    bool LogPortOpen;

    /*
     * The octet count as it stood when the link last moved, and how many pump
     * passes it has stood still since. This is parser recovery state, not
     * reporting state: the Thingy UART can contain a boot banner before H:4,
     * and an idle gap is what lets a half-built text packet be discarded before
     * the first real HCI command arrives.
     */
    uint32_t LinkIdleOctets;
    uint32_t LinkIdlePasses;

    HciTaktOs_t Runtime;
    /*
     * The part, held through its interface. The instance belongs to the port,
     * so nothing here has to know how large it is or which part it is.
     */
    HciTarget_t Target;

    uint8_t HostPacket[HCI_APP_PACKET_SIZE];
    uint8_t ControllerPacket[HCI_APP_PACKET_SIZE];
    uint8_t CommandEvent[HCI_APP_COMMAND_EVENT_SIZE];
    uint64_t SdcMem[(HCI_SDC_MEM_SIZE + 7U) / 8U];

    alignas(4) uint8_t UartRxFifoMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t UartTxFifoMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t UsbRxFifoMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t UsbTxFifoMem[HCI_APP_FIFO_MEM_SIZE];

    int LastError;
    bool Initialized;
} HciApp_t;

/*
 * The target is passed in rather than chosen here, so this layer names no
 * part. A board decides which port it has and hands over the pair.
 */
bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType, HciTarget_t Target);
void HciAppStop(HciApp_t *pApp);
void HciAppThread(void *pContext);
bool HciAppHostIsOpen(const HciApp_t *pApp);

#ifdef __cplusplus
}
#endif

#endif /* HCI_APP_H */

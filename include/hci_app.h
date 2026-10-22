/**-------------------------------------------------------------------------
@file	hci_app.h

@brief	HciController application runtime and host transport selection.

		Defines the application state used to connect the HCI controller,
		SoftDevice Controller backend, target port, runtime, UART H:4,
		CDC H:4, native USB HCI, and diagnostic log transport.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

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
#include "hci_taktos.h"
#include "hci_usb.h"
#include "usb/usbd_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A part with no USB device, such as the nRF5340 network core, builds without
 * the USB host and diagnostic classes: set HCI_APP_HAS_USB to 0. The host is
 * then the UART or the IPC link to another core.
 */
#ifndef HCI_APP_HAS_USB
#define HCI_APP_HAS_USB             1
#endif

/* A UART host. A part that reaches its host only through IPC sets it to 0. */
#ifndef HCI_APP_HAS_UART
#define HCI_APP_HAS_UART            1
#endif

/* The host is reached through an IpcIntrf, this part is the IPC owner. */
#ifndef HCI_APP_HAS_IPC
#define HCI_APP_HAS_IPC             0
#endif

#define HCI_APP_PACKET_SIZE         1024U
#define HCI_APP_COMMAND_EVENT_SIZE  260U
#define HCI_APP_FIFO_DATA_SIZE      4096U
#define HCI_APP_FIFO_MEM_SIZE       CFIFO_MEMSIZE(HCI_APP_FIFO_DATA_SIZE)
#if HCI_APP_HAS_USB
/* Diagnostic CDC input is consumed without processing. Keep two USB RX
 * packet slots; its TX log retention uses the original full FIFO. */
#define HCI_APP_LOG_RX_MEM_SIZE      \
    USB_INTRF_RXMEM_SIZE(2U, USBD_CDC_TRANS_WORDS * sizeof(uint32_t))
#endif

typedef enum
{
    HCI_APP_HOST_UART = 0,
    HCI_APP_HOST_USB = 1,
    HCI_APP_HOST_IPC = 2,
} HciAppHost_t;

/*
 * Complete host transport mode. HostType alone cannot distinguish the two USB
 * transports, and that distinction is selected at run time by the mode button.
 */
typedef enum
{
    HCI_APP_MODE_UART_H4 = 0,
    HCI_APP_MODE_USB_H4 = 1,
    HCI_APP_MODE_USB_NATIVE = 2,
    HCI_APP_MODE_IPC_H4 = 3,
} HciAppMode_t;

typedef struct
{
    HciController_t Controller;
    HciSdc_t Sdc;

    /* Which layers the vendor specific counter readout reports. */
    HciCounters_t Counters;

    /*
     * Developer trace storage is owned by the standard IOsonata SysLog and its
     * CFifo, outside this structure. This layer only supplies the diagnostic
     * CDC DeviceIntrf when USB is running. It is never mixed with the HCI stream.
     */

#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    bool BridgeUartReady;
    uint32_t BridgeBaudRate;
    uint8_t BridgePending[64];
    uint16_t BridgePendingLen;
    uint16_t BridgePendingOffset;
    uint8_t BridgeInputPending[64];
    uint16_t BridgeInputLen;
    uint16_t BridgeInputOffset;
#endif
    DevIntrf_t *pHostIntrf;
    HciAppHost_t HostType;
    HciAppMode_t Mode;
    HciUsbDescriptorMode_t UsbDescriptorMode;
    bool UsbHciNative;
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

    /* Whether a terminal has the diagnostic CDC function open. */
    bool LogPortOpen;

    /*
     * The octet count as it stood when the H:4 link last moved, and how many
     * pump passes it has stood still since. Native USB is packet-oriented and
     * does not use this parser recovery state.
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

#if HCI_APP_HAS_USB || HCI_APP_HAS_UART
    /* UART H:4 and USB HCI are exclusive host modes. These FIFO buffers
     * are reused by whichever host transport is active. The diagnostic
     * CDC and optional nRF91 UART bridge retain independent storage. An IPC
     * host keeps its rings in the shared memory block instead. */
    alignas(4) uint8_t HostRxFifoMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t HostTxFifoMem[HCI_APP_FIFO_MEM_SIZE];
#endif
#if defined(HCI_UART_BRIDGE) && HCI_UART_BRIDGE
    alignas(4) uint8_t BridgeUartRxMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t BridgeUartTxMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t BridgeUsbRxMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t BridgeUsbTxMem[HCI_APP_FIFO_MEM_SIZE];
#endif
#if HCI_APP_HAS_USB
    alignas(4) uint8_t LogRxFifoMem[HCI_APP_LOG_RX_MEM_SIZE];
    alignas(4) uint8_t LogTxFifoMem[HCI_APP_FIFO_MEM_SIZE];
#endif

    int LastError;
    bool Initialized;
} HciApp_t;

/*
 * The target is passed in rather than chosen here, so this layer names no
 * part. A board decides which port it has and hands over the pair.
 */

/*
 * Arm only the physical UART receive path before full controller startup.
 * The caller owns the object storage and must supply its cold-boot initialized
 * instance. H:4/controller processing remains deferred until HciAppInit().
 */
bool HciAppUartEarlyInit(HciApp_t *pApp, HciTarget_t Target);

/* Runtime transport selection used by the persistent board mode setting. */
bool HciAppInitMode(HciApp_t *pApp, HciAppMode_t Mode, HciTarget_t Target);

/*
 * Compatibility entry point. USB keeps the compile-time
 * HCI_USB_HCI_TRANSPORT default; UART is H:4.
 */
bool HciAppInit(HciApp_t *pApp, HciAppHost_t HostType, HciTarget_t Target);
void HciAppStop(HciApp_t *pApp);
void HciAppThread(void *pContext);
bool HciAppHostIsOpen(const HciApp_t *pApp);

#ifdef __cplusplus
}
#endif

#endif /* HCI_APP_H */
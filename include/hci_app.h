/**-------------------------------------------------------------------------
@file	hci_app.h

@brief	HciController application runtime and host transport selection.

		Defines the application state used to connect the HCI controller,
		SoftDevice Controller backend, target port, runtime and the host
		transport the project links: UART H:4 (hci_host_uart.cpp), CDC H:4
		or native USB HCI with the diagnostic log port (hci_host_usb.cpp),
		IPC to another core (hci_host_ipc.cpp).

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_APP_H
#define HCI_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "cfifo.h"
#include "device_intrf.h"
#include "hci_controller.h"
#include "hci_sdc_resources.h"
#include "hci_target.h"
#include "hci_sdc_nrfxlib.h"
#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_APP_PACKET_SIZE         1024U
#define HCI_APP_COMMAND_EVENT_SIZE  260U
#define HCI_APP_FIFO_DATA_SIZE      4096U
#define HCI_APP_FIFO_MEM_SIZE       CFIFO_MEMSIZE(HCI_APP_FIFO_DATA_SIZE)

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
    HCI_APP_MODE_IPC = 3,
} HciAppMode_t;

typedef struct
{
    HciController_t Controller;
    HciSdcNrfxlib Sdc;

    /* Which layers the vendor specific counter readout reports. */
    HciCounters_t Counters;

    /*
     * Developer trace storage is owned by the standard IOsonata SysLog and its
     * CFifo, outside this structure. This layer only supplies the diagnostic
     * CDC DeviceIntrf when USB is running. It is never mixed with the HCI stream.
     */

    DevIntrf_t *pHostIntrf;
    HciAppHost_t HostType;
    HciAppMode_t Mode;
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

    /* UART H:4 and USB HCI are exclusive host modes. These FIFO buffers
     * are reused by whichever of the two is active. An IPC host keeps its
     * rings in the shared memory block instead; the diagnostic CDC and the
     * UART bridge have storage of their own in hci_host_usb.cpp. */
    alignas(4) uint8_t HostRxFifoMem[HCI_APP_FIFO_MEM_SIZE];
    alignas(4) uint8_t HostTxFifoMem[HCI_APP_FIFO_MEM_SIZE];

    int LastError;
    bool Initialized;
} HciApp_t;

/*
 * The target is passed in rather than chosen here, so this layer names no
 * part. A board decides which port it has and hands over the pair.
 *
 * The host transports are the same: each one is a file a project links,
 * hci_host_uart.cpp, hci_host_usb.cpp or hci_host_ipc.cpp, and this layer
 * has a weak default for every entry below that reports the transport as
 * absent. A mode whose transport is not in the image fails at HciAppInitMode
 * with the host interface error; nothing here names what a board has.
 */

/* Wake the worker: a transport has data in or room out. */
void HciAppWake(void);

/* UART H:4 host. Opens the UART of board.h and sets pApp->pHostIntrf. */
bool HciHostUartInit(HciApp_t *pApp);
void HciHostUartRelease(HciApp_t *pApp);

/*
 * USB host: the device stack with the HCI function the mode asks for (none
 * for a UART or IPC host that only wants the diagnostic CDC port), the
 * diagnostic CDC and, on a board that has one, the UART bridge.
 */
bool HciHostUsbSetup(HciApp_t *pApp);
void HciHostUsbRelease(HciApp_t *pApp);
/* Enable the bus and wait for the host to configure the device. */
bool HciHostUsbStart(HciApp_t *pApp);
/* The log port alone, for a UART or IPC host on a board with a socket. */
void HciHostUsbStartLogPort(HciApp_t *pApp);
/* One pass: queued USB work, host open state, log port, bridge. */
void HciHostUsbProcess(HciApp_t *pApp);
bool HciHostUsbIsOpen(const HciApp_t *pApp);
/* The host DeviceIntrf of the USB mode in use, CDC H:4 or native. */
DevIntrf_t *HciHostUsbIntrf(const HciApp_t *pApp);
/* Native HCI over USB is a packet transport, CDC H:4 a byte stream. */
bool HciHostUsbIsPacket(const HciApp_t *pApp);

/* IPC host on another core, a packet transport. */
bool HciHostIpcInit(HciApp_t *pApp);
void HciHostIpcRelease(HciApp_t *pApp);

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
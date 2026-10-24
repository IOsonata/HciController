/**-------------------------------------------------------------------------
@file	board.h

@brief	nRF5340 network core HciController configuration.

		The network core has no USB and no UART wired to a host. The only
		host transport is the IPC memory block shared with the application
		core. HciController owns that block: it partitions and initializes
		it at start, and the application core attaches as the peer.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#ifndef __BOARD_H__
#define __BOARD_H__

/* IDs are local to the nRF5340 network core target. */
#define NRF5340_NET_GENERIC 300

#ifndef BOARD
#define BOARD NRF5340_NET_GENERIC
#endif

#define HCI_HOST_SELECT_AUTO 1
#define HCI_HOST_SELECT_USB 2
#define HCI_HOST_SELECT_UART 3

#define BOARD_NAME "nRF5340 network core"
#define BOARD_MODULE_NAME "Nordic nRF5340"

#define HCI_USB_SOCKET 0
#define HCI_STATUS_LEDS 0
#define HCI_UART_EARLY_STARTUP 0
#define HCI_BOARD_HAS_UART 1

/*
 * Two host transports, swapped at run time: IPC to the application core
 * (default) or H:4 over a UART for a bench host. A press of the mode button
 * stops the controller, stores the other mode in the retained registers and
 * resets the network core. The mode is lost on power loss and IPC is used
 * again.
 *
 * The application core must hand the button and UART pins to the network
 * core (GPIO MCUSEL) before it starts the network core. Pins below are for
 * the nRF5340 Audio DK by default. Define NRF5340_DK_PCA10095 for the
 * standard nRF5340 DK. Change them for another board.
 */
#define HCI_MODE_SWITCH 1

#if defined(NRF5340_DK_PCA10095)
/* nRF5340 DK: button 1 P0.23, virtual COM P0.19 RTS, P0.20 TXD, P0.21 CTS, P0.22 RXD */
#define HCI_MODE_BUTTON_PORT 0
#define HCI_MODE_BUTTON_PIN 23
#define HCI_MODE_BUTTON_PINOP 0
#define UART_RX_PORT 0
#define UART_RX_PIN 22
#define UART_TX_PORT 0
#define UART_TX_PIN 20
#define UART_CTS_PORT 0
#define UART_CTS_PIN 21
#define UART_RTS_PORT 0
#define UART_RTS_PIN 19
#else
/*
 * nRF5340 Audio DK: button VOL- P0.02; the network core's own virtual COM
 * port, the second one the board enumerates: P1.09 TXD, P1.08 RXD,
 * P1.10 RTS, P1.11 CTS (Zephyr nrf5340_audio_dk_nrf5340_cpunet-pinctrl).
 * The first port, P1.05/P1.04, is the application core console and stays
 * with it.
 */
#define HCI_MODE_BUTTON_PORT 0
#define HCI_MODE_BUTTON_PIN 2
#define HCI_MODE_BUTTON_PINOP 0
#define UART_RX_PORT 1
#define UART_RX_PIN 8
#define UART_TX_PORT 1
#define UART_TX_PIN 9
#define UART_CTS_PORT 1
#define UART_CTS_PIN 11
#define UART_RTS_PORT 1
#define UART_RTS_PIN 10
#endif

#define UART_DEVNO 0
#define UART_RX_PINOP 1
#define UART_TX_PINOP 1
#define UART_CTS_PINOP 1
#define UART_RTS_PINOP 1
#define UART_RATE 1000000
#if defined(NRF5340_DK_PCA10095)
#define UART_HW_FLOWCTRL 1
#define UART_FLOWCTRL UART_FLWCTRL_HW
#else
/* The Audio DK virtual COM port runs without flow control, as the IPC test does. */
#define UART_HW_FLOWCTRL 0
#define UART_FLOWCTRL UART_FLWCTRL_NONE
#endif

/*
 * Shared memory block used by the IPC host transport.
 *
 * The block lives in application core RAM (the network core sees it at the
 * same 0x2000_0000 based address). It must not overlap anything the
 * application core image uses, so reserve it in the application linker
 * script. The application core must also open the region to the network
 * core (SPU RAM permissions) before it releases the network core from reset.
 *
 * HCI_IPC_MEM_SIZE is the room reserved. The owner uses only what
 * BT_HCI_IPC_MEMSIZE() needs for HCI_IPC_NB_PKT packet slots in each
 * direction.
 */
#define HCI_IPC_MEM_ADDR 0x20070000UL
#define HCI_IPC_MEM_SIZE 0x10000UL
#define HCI_IPC_NB_PKT 8U

/* Controller to host uses channel 0, host to controller uses channel 1. */
#define HCI_IPC_TX_CHAN 0
#define HCI_IPC_RX_CHAN 1
#define HCI_IPC_IRQ_PRIORITY 6

#define UART_PINS { \
    {UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

/* Low frequency clock of the network core. The 32 kHz crystal needs its load
 * capacitance set from the application core, and mpsl_init waits for the
 * clock to run, so a crystal that never starts holds the controller in
 * mpsl_init. The RC oscillator starts by itself. Define HCI_LFCLK_XTAL to
 * use the crystal once the application core sets it up. */
#ifndef HCI_LFCLK_XTAL
#define MCU_OSC { {OSC_TYPE_XTAL, 32000000, 20, 0}, {OSC_TYPE_RC, 32768, 250, 0}, false }
#endif

#endif /* __BOARD_H__ */

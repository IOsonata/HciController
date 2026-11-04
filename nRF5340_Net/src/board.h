/**-------------------------------------------------------------------------
@file	board.h

@brief	nRF5340 network core boards supported by HciController.

		The firmware runs on the network core, which has no USB. The HCI port
		is IPC, through a memory block shared with the application core.
		HciController owns that block: it partitions and initializes it at
		start, and the application core attaches as the peer.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef __BOARD_H__
#define __BOARD_H__

/* IDs are local to the nRF5340 network core target. */
#define NRF5340_AUDIO_DK_NET		300
#define NRF5340_DK_NET				301
#define THINGY91X_NRF5340			302

/* -DBOARD=... on the command line wins. */
#ifndef BOARD
#define BOARD						NRF5340_AUDIO_DK_NET
#endif

#define HCI_HOST_SELECT_AUTO		1
#define HCI_HOST_SELECT_USB			2
#define HCI_HOST_SELECT_UART		3

#define HCI_USB_SOCKET				0
#define HCI_STATUS_LEDS				0
#define HCI_UART_EARLY_STARTUP		0

/*
 * A board with HCI_MODE_SWITCH has two host transports, swapped at run time:
 * IPC to the application core (default) or H:4 over the UART for a bench host.
 * A press of the mode button stops the controller, stores the other mode in
 * the retained registers and resets the network core. The mode is lost on
 * power loss and IPC is used again. In IPC mode the UART carries the debug log.
 *
 * The application core must hand the button and UART pins to the network core
 * (GPIO MCUSEL) before it starts the network core.
 */

#if BOARD == NRF5340_AUDIO_DK_NET

#define BOARD_NAME					"nRF5340 Audio DK network core"

#define HCI_MODE_SWITCH				1
#define HCI_BOARD_HAS_UART			1

/* Mode button VOL- P0.02. */
#define HCI_MODE_BUTTON_PORT		0
#define HCI_MODE_BUTTON_PIN			2
#define HCI_MODE_BUTTON_PINOP		0

/*
 * The network core's own virtual COM port, the second one the board enumerates
 * (Zephyr nrf5340_audio_dk_nrf5340_cpunet-pinctrl). The first port, P1.05/P1.04,
 * is the application core console and stays with it.
 */
#define UART_TX_PORT				1
#define UART_TX_PIN					9
#define UART_RX_PORT				1
#define UART_RX_PIN					8
#define UART_RTS_PORT				1
#define UART_RTS_PIN				10
#define UART_CTS_PORT				1
#define UART_CTS_PIN				11

/* The virtual COM port runs without flow control, as the IPC test does. */
#define UART_HW_FLOWCTRL			0
#define UART_FLOWCTRL				UART_FLWCTRL_NONE

#elif BOARD == NRF5340_DK_NET

#define BOARD_NAME					"nRF5340 DK network core"

#define HCI_MODE_SWITCH				1
#define HCI_BOARD_HAS_UART			1

/* Mode button 1 P0.23. */
#define HCI_MODE_BUTTON_PORT		0
#define HCI_MODE_BUTTON_PIN			23
#define HCI_MODE_BUTTON_PINOP		0

/* Virtual COM port. */
#define UART_TX_PORT				0
#define UART_TX_PIN					20
#define UART_RX_PORT				0
#define UART_RX_PIN					22
#define UART_RTS_PORT				0
#define UART_RTS_PIN				19
#define UART_CTS_PORT				0
#define UART_CTS_PIN				21

#define UART_HW_FLOWCTRL			1
#define UART_FLOWCTRL				UART_FLWCTRL_HW

#elif BOARD == THINGY91X_NRF5340

#define BOARD_NAME					"Nordic Thingy:91 X network core"

/*
 * The HCI port is IPC only. Nordic's board files define no UART for the network
 * core: the nRF5340 UART pins, P0.29/P1.04 and P1.08/P1.06, are the links to the
 * nRF9151.
 *
 * The application core must enable the short range RF front end, P0.18 active
 * low, before the radio is used.
 */
#define HCI_MODE_SWITCH				0
#define HCI_BOARD_HAS_UART			0

#else
#error "Unsupported nRF5340 network core board"
#endif

#if HCI_BOARD_HAS_UART

#define UART_DEVNO					0
#define UART_RATE					1000000
#define UART_TX_PINOP				1
#define UART_RX_PINOP				1
#define UART_RTS_PINOP				1
#define UART_CTS_PINOP				1

/* RX and TX come first. hci_host_uart.cpp opens only those two for the log. */
#define UART_PINS	{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#endif /* HCI_BOARD_HAS_UART */

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
#define HCI_IPC_MEM_ADDR			0x20070000UL
#define HCI_IPC_MEM_SIZE			0x10000UL
#define HCI_IPC_NB_PKT				8U

/* Controller to host uses channel 0, host to controller uses channel 1. */
#define HCI_IPC_TX_CHAN				0
#define HCI_IPC_RX_CHAN				1
#define HCI_IPC_IRQ_PRIORITY		6

/*
 * Low frequency clock of the network core. The 32 kHz crystal needs its load
 * capacitance set from the application core, and mpsl_init waits for the clock
 * to run, so a crystal that never starts holds the controller in mpsl_init. The
 * RC oscillator starts by itself. Define HCI_LFCLK_XTAL to use the crystal once
 * the application core sets it up.
 */
#ifndef HCI_LFCLK_XTAL
#define MCU_OSC	{ {OSC_TYPE_XTAL, 32000000, 20, 0}, {OSC_TYPE_RC, 32768, 250, 0}, false }
#endif

#endif /* __BOARD_H__ */

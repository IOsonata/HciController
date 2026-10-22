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
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 0
#define HCI_UART_EARLY_STARTUP 0
#define HCI_BOARD_HAS_UART 0

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
 * IPC_INTRF_MEMSIZE() needs for two HCI_IPC_RING_BYTES byte rings.
 */
#define HCI_IPC_MEM_ADDR 0x20070000UL
#define HCI_IPC_MEM_SIZE 0x10000UL
#define HCI_IPC_RING_BYTES 2048U

/* Controller to host uses channel 0, host to controller uses channel 1. */
#define HCI_IPC_TX_CHAN 0
#define HCI_IPC_RX_CHAN 1
#define HCI_IPC_IRQ_PRIORITY 6

#endif /* __BOARD_H__ */

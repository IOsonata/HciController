/**-------------------------------------------------------------------------
@file	board.h

@brief	nRF52840 boards supported by HciController.

		One branch per board. What a branch leaves out takes the default at
		the end of this file.

		A pin is three macros, <NAME>_PORT, <NAME>_PIN and <NAME>_PINOP.
		PINOP selects the pin function on other MCUs. The Nordic IOPinCfg
		ignores it.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef __BOARD_H__
#define __BOARD_H__

#include "blyst840_boards.h"

/* IBK_NRF52840 and UDG_NRF52840 come from blyst840_boards.h, which uses 1 to 6. */
#define THINGY91_NRF52840			100
#define WILDTHING51					101
#define WILDTHING91					102

/* -DBOARD=... on the command line wins. */
#ifndef BOARD
#define BOARD						UDG_NRF52840
#endif

/*
 * Only for a board with no 32768 Hz crystal. SystemCoreClockGet feeds the
 * TaktOS tick rate, so a wrong high frequency value scales every timeout.
 */
// #define MCU_OSC	{ {OSC_TYPE_XTAL, 32000000, 20, 100}, {OSC_TYPE_RC, 32768, 250, 0}, false }

/*
 * HCI_HOST_SELECT picks the mode used when no saved mode exists. Which modes
 * are legal is decided per board in main.cpp, whatever this says.
 *
 *   HCI_HOST_SELECT_AUTO   VBUS chooses between USB and UART where legal
 *   HCI_HOST_SELECT_USB    the configured USB transport
 *   HCI_HOST_SELECT_UART   UART H:4
 */
#define HCI_HOST_SELECT_AUTO		1
#define HCI_HOST_SELECT_USB			2
#define HCI_HOST_SELECT_UART		3

#if BOARD == UDG_NRF52840

#define BOARD_NAME					"I-SYST UDG-NRF52840x Dongle"

/* USB only: the button cycles USB H:4 and native USB HCI. No UART pins. */
#define HCI_USB_SOCKET				1
#define HCI_HOST_SELECT				HCI_HOST_SELECT_AUTO
#define HCI_BOARD_HAS_UART			0

#define HCI_MODE_SWITCH				1
#define HCI_MODE_BUTTON_PORT		UDG_NRF52840_BUT1_PORT
#define HCI_MODE_BUTTON_PIN			UDG_NRF52840_BUT1_PIN
#define HCI_MODE_BUTTON_PINOP		UDG_NRF52840_BUT1_PINOP

#define HCI_STATUS_LEDS				1
#define LED_PINS					UDG_NRF52840_LED_PINS_CFG
#define HCI_LED_RED_PORT			UDG_NRF52840_LEDR_PORT
#define HCI_LED_RED_PIN				UDG_NRF52840_LEDR_PIN
#define HCI_LED_RED_ACTIVE			UDG_NRF52840_LEDR_ACTIVE
#define HCI_LED_GREEN_PORT			UDG_NRF52840_LEDG_PORT
#define HCI_LED_GREEN_PIN			UDG_NRF52840_LEDG_PIN
#define HCI_LED_GREEN_ACTIVE		UDG_NRF52840_LEDG_ACTIVE
#define HCI_LED_BLUE_PORT			UDG_NRF52840_LEDB_PORT
#define HCI_LED_BLUE_PIN			UDG_NRF52840_LEDB_PIN
#define HCI_LED_BLUE_ACTIVE			UDG_NRF52840_LEDB_ACTIVE

#elif BOARD == IBK_NRF52840

#define BOARD_NAME					"I-SYST IBK-NRF52840"

/* The button cycles UART H:4, USB H:4 and native USB HCI. */
#define HCI_USB_SOCKET				1
#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT				HCI_HOST_SELECT_AUTO
#endif

#define HCI_MODE_SWITCH				1
#define HCI_MODE_BUTTON_PORT		IBK_NRF52840_BUT1_PORT
#define HCI_MODE_BUTTON_PIN			IBK_NRF52840_BUT1_PIN
#define HCI_MODE_BUTTON_PINOP		IBK_NRF52840_BUT1_PINOP

#define HCI_STATUS_LEDS				1
#define LED_PINS					IBK_NRF52840_LED_PINS_CFG
#define HCI_LED_RED_PORT			IBK_NRF52840_LED3_PORT
#define HCI_LED_RED_PIN				IBK_NRF52840_LED3_PIN
#define HCI_LED_RED_ACTIVE			IBK_NRF52840_LED3_LOGIC
#define HCI_LED_GREEN_PORT			IBK_NRF52840_LED2_PORT
#define HCI_LED_GREEN_PIN			IBK_NRF52840_LED2_PIN
#define HCI_LED_GREEN_ACTIVE		IBK_NRF52840_LED2_LOGIC
#define HCI_LED_BLUE_PORT			IBK_NRF52840_LED1_PORT
#define HCI_LED_BLUE_PIN			IBK_NRF52840_LED1_PIN
#define HCI_LED_BLUE_ACTIVE			IBK_NRF52840_LED1_LOGIC

/*
 * Breakout: nothing on the board claims a UART, so these pins are only the
 * firmware defaults. Change them to match the bench wiring. RTS and CTS are
 * not driven until UART_HW_FLOWCTRL is set to 1.
 */
#define UART_DEVNO					0
#define UART_RATE					1000000

#define UART_TX_PORT				0
#define UART_TX_PIN					25
#define UART_TX_PINOP				0
#define UART_RX_PORT				1
#define UART_RX_PIN					0
#define UART_RX_PINOP				0
#define UART_RTS_PORT				0
#define UART_RTS_PIN				19
#define UART_RTS_PINOP				0
#define UART_CTS_PORT				0
#define UART_CTS_PIN				22
#define UART_CTS_PINOP				0

#elif BOARD == THINGY91_NRF52840

#define BOARD_NAME					"Nordic Thingy:91"

/*
 * The HCI host is the nRF9160 over the interconnect UART. The USB socket only
 * carries the log and the bridge to the nRF9160 console. No LED reaches this
 * part. Its one button, P1.13, is not used.
 *
 * Interface    nRF9160                 nRF52840
 * HCI UART1    TX P0.22, RX P0.23      TX P0.25, RX P1.00
 *              RTS P0.24, CTS P0.25    RTS P0.22, CTS P0.19
 *              1000000 baud, hardware flow control
 * Debug UART0  TX P0.18, RX P0.19      TX P0.15, RX P0.11
 *              115200 baud, no flow control
 */
#define HCI_USB_SOCKET				1
#define HCI_HOST_SELECT				HCI_HOST_SELECT_UART
#define HCI_UART_EARLY_STARTUP		1
#define HCI_H4_STARTUP_RESET_SYNC	1
#define HCI_SDC_STARTUP_NOP			1

#define UART_DEVNO					1
#define UART_RATE					1000000
#define UART_HW_FLOWCTRL			1

#define UART_TX_PORT				0
#define UART_TX_PIN					25
#define UART_TX_PINOP				0
#define UART_RX_PORT				1
#define UART_RX_PIN					0
#define UART_RX_PINOP				0
#define UART_RTS_PORT				0
#define UART_RTS_PIN				22
#define UART_RTS_PINOP				0
#define UART_CTS_PORT				0
#define UART_CTS_PIN				19
#define UART_CTS_PINOP				0

#define HCI_UART_BRIDGE				1
#define HCI_BRIDGE_UART_DEVNO		0
#define HCI_BRIDGE_UART_RATE		115200
#define HCI_BRIDGE_UART_PINS	{ \
	{0, 11, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{0, 15, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#elif BOARD == WILDTHING91

#define BOARD_NAME					"I-SYST WildThing91"

/*
 * HCI is always UART, with the same UART pin map and debug bridge as Thingy:91.
 * The nRF91 reset line does not reach this part, so there is no early UART
 * startup.
 */
#define HCI_USB_SOCKET				1
#define HCI_HOST_SELECT				HCI_HOST_SELECT_UART
#define HCI_H4_STARTUP_RESET_SYNC	1

#define UART_DEVNO					1
#define UART_RATE					1000000
#define UART_HW_FLOWCTRL			1

#define UART_TX_PORT				0
#define UART_TX_PIN					25
#define UART_TX_PINOP				0
#define UART_RX_PORT				1
#define UART_RX_PIN					0
#define UART_RX_PINOP				0
#define UART_RTS_PORT				0
#define UART_RTS_PIN				22
#define UART_RTS_PINOP				0
#define UART_CTS_PORT				0
#define UART_CTS_PIN				19
#define UART_CTS_PINOP				0

#define HCI_UART_BRIDGE				1
#define HCI_BRIDGE_UART_DEVNO		0
#define HCI_BRIDGE_UART_RATE		115200
#define HCI_BRIDGE_UART_PINS	{ \
	{0, 11, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{0, 15, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#elif BOARD == WILDTHING51

#define BOARD_NAME					"I-SYST WildThing51"

/*
 * The four BTLTE nets are two UART pairs, with no RTS or CTS:
 *
 * HCI 1 Mbaud      BTLTE0 P0.23 RX, BTLTE1 P0.24 TX
 * Trace 115200     BTLTE2 P0.21 RX, BTLTE3 P1.04 TX, forwarded to USB CDC
 *
 * nRF9151 firmware must configure P0.02 as trace TX and P0.03 as trace RX.
 */
#define HCI_USB_SOCKET				1
#define HCI_HOST_SELECT				HCI_HOST_SELECT_UART
#define HCI_UART_EARLY_STARTUP		1
#define HCI_H4_STARTUP_RESET_SYNC	1

#define UART_DEVNO					0
#define UART_RATE					1000000
#define UART_HW_FLOWCTRL			0

#define UART_TX_PORT				0
#define UART_TX_PIN					24
#define UART_TX_PINOP				0
#define UART_RX_PORT				0
#define UART_RX_PIN					23
#define UART_RX_PINOP				0

#define HCI_UART_BRIDGE				1
#define HCI_BRIDGE_UART_DEVNO		1
#define HCI_BRIDGE_UART_RATE		115200
#define HCI_BRIDGE_UART_PINS	{ \
	{0, 21, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{1, 4, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#else
#error "No pins defined. Define the pins used by your board."
#endif

/*
 * Defaults for what a branch leaves out.
 *
 * HCI_MODE_SWITCH            a button cycles the host mode
 * HCI_STATUS_LEDS            RGB status LED
 * HCI_UART_EARLY_STARTUP     open the host UART first thing in main, for a
 *                            host that can reset this part
 * HCI_H4_STARTUP_RESET_SYNC  discard what the UART receives before the first
 *                            HCI Reset, such as boot text
 * HCI_BOARD_HAS_UART         0 on a board with no UART pins
 */
#ifndef HCI_MODE_SWITCH
#define HCI_MODE_SWITCH				0
#endif

#ifndef HCI_STATUS_LEDS
#define HCI_STATUS_LEDS				0
#endif

#ifndef HCI_UART_EARLY_STARTUP
#define HCI_UART_EARLY_STARTUP		0
#endif

#ifndef HCI_H4_STARTUP_RESET_SYNC
#define HCI_H4_STARTUP_RESET_SYNC	0
#endif

#ifndef HCI_BOARD_HAS_UART
#define HCI_BOARD_HAS_UART			1
#endif

#if HCI_BOARD_HAS_UART

/*
 * Built once for every branch so the two agree. A board sets UART_HW_FLOWCTRL
 * to 1 and names RTS and CTS to get both. RX and TX come first,
 * hci_host_uart.cpp depends on it.
 */
#ifndef UART_HW_FLOWCTRL
#define UART_HW_FLOWCTRL			0
#endif

#if UART_HW_FLOWCTRL

#if !defined(UART_RTS_PORT) || !defined(UART_CTS_PORT)
#error "UART_HW_FLOWCTRL needs UART_RTS_PORT/PIN/PINOP and UART_CTS_PORT/PIN/PINOP from the board"
#endif

#define UART_FLOWCTRL				UART_FLWCTRL_HW
#define UART_PINS	{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#else

#define UART_FLOWCTRL				UART_FLWCTRL_NONE
#define UART_PINS	{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#endif

#endif /* HCI_BOARD_HAS_UART */

#endif /* __BOARD_H__ */

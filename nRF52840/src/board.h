/**-------------------------------------------------------------------------
@file	board.h

@brief	nRF52840 board pin assignments and HciController board policy.

		Defines board-specific LEDs, buttons, UART pins, host transport policy,
		mode-switch support, startup behavior, and UART flow-control mapping
		for the supported nRF52840-based products and development boards.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef __BOARD_H__
#define __BOARD_H__

#include "blyst840_boards.h"

// Uncomment and set the MCU oscillator used if different from default SystemInit
// The nRF52840 high frequency crystal is 32 MHz. SystemCoreClockGet feeds the
// TaktOS tick rate, so a wrong value here scales every timeout.
// Uncomment only on a board with no 32768 Hz crystal.
// #define MCU_OSC			{ {OSC_TYPE_XTAL, 32000000, 20, 100}, {OSC_TYPE_RC,	32768, 250, 0}, false }

// Each IO pin has three associated macros:
//   <DEV_PIN_NAME>_PORT   : GPIO port number
//   <DEV_PIN_NAME>_PIN    : GPIO pin number
//   <DEV_PIN_NAME>_PINOP  : pin operation/option (MCU specific; 0 is GPIO)
//
// Notes:
// - Nordic nRF52/nRF54: PINOP is not used; keep it at 0.
// - Many MCUs (e.g., STM32) require a non-zero PINOP for alternate functions
//   (UART/SPI/I2C). Choose the correct AF/PINOP for your selected pins.


/*
 * The boards below are I-SYST hardware, and their ids come from IOsonata
 * blyst840_boards.h. A port to something else is a new id, a new branch in the
 * #if chain, and the pins and clock source that board actually has. Ids 1 to 6
 * are taken by that header, so a local one wants to sit well clear of them.
 */

/*
 * Nordic Thingy:91. Not I-SYST hardware, so its id sits well clear of the
 * IOsonata range rather than pretending to belong to it. The nRF52840 on that
 * board reaches its host over the interconnect UART to the nRF9160, never over
 * USB, so it is the one board here where VBUS decides nothing about the host.
 * The socket is still this part's and is where the log goes.
 */
#define THINGY91_NRF52840	100

/* Application-local ids until these products are added to IOsonata. */
#define WILDTHING51		101
#define WILDTHING91		102

/* -DBOARD=... on the command line wins, which is how the build is checked
 * against every board without editing this file. */
#ifndef BOARD
#define BOARD			UDG_NRF52840
//#define BOARD			IBK_NRF52840
//#define BOARD			THINGY91_NRF52840
#endif

//=============================================================================
// Host port selection
//=============================================================================

/*
 * Which port supplies the default HCI mode when no saved mode exists.
 *
 *   HCI_HOST_SELECT_AUTO   VBUS chooses the initial USB/UART family
 *   HCI_HOST_SELECT_USB    initial mode is the configured USB transport
 *   HCI_HOST_SELECT_UART   initial mode is UART/H:4
 *
 * On boards with HCI_MODE_SWITCH, the selected mode is subsequently stored in
 * NVM and survives reset/power cycle. Board policy still limits which modes are
 * legal; for example UDG never allows UART-HCI regardless of this default.
 */
#define HCI_HOST_SELECT_AUTO	1
#define HCI_HOST_SELECT_USB		2
#define HCI_HOST_SELECT_UART	3

#if BOARD == UDG_NRF52840

#define BOARD_NAME                      "I-SYST UDG-NRF52840x Dongle"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

/* The USB socket is this part's own. HCI itself is USB-only on UDG. */
#define HCI_USB_SOCKET                  1
#define HCI_MODE_SWITCH                 1
#define HCI_MODE_BUTTON_PORT            UDG_NRF52840_BUT1_PORT
#define HCI_MODE_BUTTON_PIN             UDG_NRF52840_BUT1_PIN
#define HCI_MODE_BUTTON_PINOP           UDG_NRF52840_BUT1_PINOP

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_AUTO
#endif

#define HCI_LED_RED_PORT                UDG_NRF52840_LEDR_PORT
#define HCI_LED_RED_PIN                 UDG_NRF52840_LEDR_PIN
#define HCI_LED_RED_ACTIVE              UDG_NRF52840_LEDR_ACTIVE

#define HCI_LED_GREEN_PORT              UDG_NRF52840_LEDG_PORT
#define HCI_LED_GREEN_PIN               UDG_NRF52840_LEDG_PIN
#define HCI_LED_GREEN_ACTIVE            UDG_NRF52840_LEDG_ACTIVE

#define HCI_LED_BLUE_PORT               UDG_NRF52840_LEDB_PORT
#define HCI_LED_BLUE_PIN                UDG_NRF52840_LEDB_PIN
#define HCI_LED_BLUE_ACTIVE             UDG_NRF52840_LEDB_ACTIVE

#define LED_PINS						UDG_NRF52840_LED_PINS_CFG

//=============================================================================
// Button Pin Definitions (default for nRF52840)
//=============================================================================

// The dongle has one button, P1.06. The four button block that used to sit
// here carried IBK pin numbers: BUTTON1 P0.13 and BUTTON2 P0.04 are the IBK
// buttons, and BUTTON3 and BUTTON4 were both P0.00, which is XL1, the low
// frequency crystal pin. Nothing read them, and driving XL1 would stop the
// crystal, so they are gone rather than left as a trap.

#define BUTTON1_PORT					UDG_NRF52840_BUT1_PORT
#define BUTTON1_PIN						UDG_NRF52840_BUT1_PIN
#define BUTTON1_PINOP					UDG_NRF52840_BUT1_PINOP

#define BUTTON_PINS						UDG_NRF52840_BUT_PINS_CFG


//=============================================================================
// UART Pin Definitions
//=============================================================================

/*
 * Placeholders, and they were worse than that. P0.25, P1.00, P0.19 and P0.22
 * are not dongle pins at all: they are the Nordic Thingy:91 nRF52840
 * interconnect, copied here and then left. The Thingy:91 branch below has that
 * board's measured mapping.
 *
 * These pins remain board/header metadata, but HciController never selects
 * UART-HCI for UDG. main.cpp rejects a forced UART mode on this board.
 */
#define UART_TX_PORT            0
#define UART_TX_PIN             24
#define UART_TX_PINOP           0

#define UART_RX_PORT            0
#define UART_RX_PIN             23
#define UART_RX_PINOP           0

#define UART_RTS_PORT           0
#define UART_RTS_PIN            19
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            22
#define UART_CTS_PINOP          0

#define UART_DEVNO			0

#define UART_RATE			1000000


#elif BOARD == IBK_NRF52840

#define BOARD_NAME                      "I-SYST IBK-NRF52840"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

/* The USB socket is this part's own, so VBUS is worth reading for first boot. */
#define HCI_USB_SOCKET                  1
#define HCI_MODE_SWITCH                 1
#define HCI_MODE_BUTTON_PORT            IBK_NRF52840_BUT1_PORT
#define HCI_MODE_BUTTON_PIN             IBK_NRF52840_BUT1_PIN
#define HCI_MODE_BUTTON_PINOP           IBK_NRF52840_BUT1_PINOP

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_AUTO
#endif

#define BUTTON1_PINS					IBK_NRF52840_BUT_PINS_CFG

/* Product status LED channels. Polarity comes from the IOsonata board definitions. */

#define HCI_LED_RED_PORT                IBK_NRF52840_LED3_PORT
#define HCI_LED_RED_PIN                 IBK_NRF52840_LED3_PIN
#define HCI_LED_RED_ACTIVE              IBK_NRF52840_LED3_LOGIC

#define HCI_LED_GREEN_PORT              IBK_NRF52840_LED2_PORT
#define HCI_LED_GREEN_PIN               IBK_NRF52840_LED2_PIN
#define HCI_LED_GREEN_ACTIVE            IBK_NRF52840_LED2_LOGIC

#define HCI_LED_BLUE_PORT               IBK_NRF52840_LED1_PORT
#define HCI_LED_BLUE_PIN                IBK_NRF52840_LED1_PIN
#define HCI_LED_BLUE_ACTIVE             IBK_NRF52840_LED1_LOGIC

#define LED_PINS						IBK_NRF52840_LED_PINS_CFG

//=============================================================================
// UART Pin Definitions
//=============================================================================

/*
 * A default rather than a constraint. This is a breakout: the module's I/O
 * comes out to headers and nothing on the board claims a UART, so any four
 * free pins will do and these four are only the ones the firmware arrives
 * with. Change them to whatever the wiring on the bench is; there is no
 * schematic to check them against.
 *
 * RTS and CTS are named even though UART_HW_FLOWCTRL is left at 0, so turning
 * flow control on is one line and not a pin hunt. Nothing drives them until
 * it is turned on.
 *
 * These four numbers are not arbitrary. They are the Nordic Thingy:91
 * nRF52840 interconnect, and they reached three board branches by being copied
 * between them. They were called crossed here for a while, on the strength of
 * the sdk-nrf pinctrl node, and they are not: measured on the board, RTS on
 * P0.19 is what makes the peer transmit. The Thingy:91 branch below has the
 * same four and says what was measured.
 *
 * This branch is still a breakout and its pins are still whatever the bench is
 * wired to. A Thingy:91 build wants BOARD=THINGY91_NRF52840.
 */
#define UART_TX_PORT            0
#define UART_TX_PIN             25
#define UART_TX_PINOP           0

#define UART_RX_PORT            1
#define UART_RX_PIN             0
#define UART_RX_PINOP           0

#define UART_RTS_PORT           0
#define UART_RTS_PIN            19
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            22
#define UART_CTS_PINOP          0

#define UART_DEVNO			0

#define UART_RATE			1000000


#elif BOARD == THINGY91_NRF52840

#define BOARD_NAME                      "Nordic Thingy:91"
#define BOARD_MODULE_NAME               "Nordic nRF52840"

/*
 * The host is the nRF9160 on the same board, over the interconnect UART. The
 * USB socket on this board belongs to the nRF52840, but nothing on the far
 * side of it speaks HCI, so VBUS decides nothing here and AUTO would come up
 * talking to a host that is not there.
 *
 * The socket is still this part's, so it is where the log goes. That is the
 * only way anything on this board can be observed: no LED reaches this part,
 * the UART is the host's, and semihosting needs a debugger on a board that is
 * usually sealed.
 */
#define HCI_USB_SOCKET                  1
#define HCI_MODE_SWITCH                 0
#define HCI_UART_EARLY_STARTUP          1
#define HCI_H4_STARTUP_RESET_SYNC       1

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_UART
#endif

/*
 * No LED reaches this part. The Thingy:91 LEDs are driven from the nRF9160
 * side, so anything driven from here would be driving pins that belong to
 * something else.
 */
#define HCI_STATUS_LEDS                 0

/* The one button this part has, from the board's own device tree. */
#define BUTTON1_PORT					1
#define BUTTON1_PIN						13
#define BUTTON1_PINOP					0

#define BUTTON_PINS { \
	{BUTTON1_PORT, BUTTON1_PIN, BUTTON1_PINOP, IOPINDIR_INPUT, IOPINRES_PULLUP, IOPINTYPE_NORMAL},}

//=============================================================================
// Nordic Thingy:91 UART mapping
//=============================================================================

/*
 * Nordic Thingy:91 reference configuration:
 *
 * Interface        nRF9160                         nRF52840
 * Debug UART0      TX P0.18, RX P0.19             TX P0.15, RX P0.11
 *                  RTS P0.20, CTS P0.21          (no flow control)
 *                  115200 baud, no flow control
 *
 * HCI UART1        TX P0.22, RX P0.23             TX P0.25, RX P1.00
 *                  RTS P0.24, CTS P0.25          RTS P0.22, CTS P0.19
 *                  1000000 baud, hardware flow control
 *
 * Source: Nordic Thingy:91 pinctrl mapping supplied for this project.
 * Do not confuse the debug UART0 console with the UART1 HCI link.
 *
 * This deliberately follows the Nordic UART1 RTS/CTS mapping. Previous
 * Thingy:91 hardware tests in this project reported the opposite RTS/CTS
 * behavior (RTS P0.19, CTS P0.22). The discrepancy still needs an on-board
 * verification before treating the changed HCI link as validated.
 */
#define THINGY91_DEBUG_UART_DEVNO  0
/* Enable the second UART0 -> independent USB CDC bridge, not HCI transport. */
#define HCI_NRF91_TRACE_BRIDGE 1
#define HCI_NRF91_TRACE_UART_DEVNO THINGY91_DEBUG_UART_DEVNO
#define HCI_NRF91_TRACE_UART_RATE THINGY91_DEBUG_UART_RATE
#define HCI_NRF91_TRACE_UART_PINS { \
    {THINGY91_DEBUG_UART_RX_PORT, THINGY91_DEBUG_UART_RX_PIN, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {THINGY91_DEBUG_UART_TX_PORT, THINGY91_DEBUG_UART_TX_PIN, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}
#define THINGY91_DEBUG_UART_TX_PORT 0
#define THINGY91_DEBUG_UART_TX_PIN  15
#define THINGY91_DEBUG_UART_RX_PORT 0
#define THINGY91_DEBUG_UART_RX_PIN  11
#define THINGY91_DEBUG_UART_RATE    115200

#define UART_TX_PORT            0
#define UART_TX_PIN             25
#define UART_TX_PINOP           0

#define UART_RX_PORT            1
#define UART_RX_PIN             0
#define UART_RX_PINOP           0

#define UART_RTS_PORT           0
#define UART_RTS_PIN            22
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            19
#define UART_CTS_PINOP          0

#define UART_HW_FLOWCTRL        1
#define UART_DEVNO              1
#define UART_RATE               1000000

/*
 * Signal-only counterpart mapping on the nRF9160, for reference.
 * These pins are not configured by the nRF52840 firmware.
 */
#define THINGY91_NRF9160_HCI_TX_PORT  0
#define THINGY91_NRF9160_HCI_TX_PIN   22
#define THINGY91_NRF9160_HCI_RX_PORT  0
#define THINGY91_NRF9160_HCI_RX_PIN   23
#define THINGY91_NRF9160_HCI_RTS_PORT 0
#define THINGY91_NRF9160_HCI_RTS_PIN  24
#define THINGY91_NRF9160_HCI_CTS_PORT 0
#define THINGY91_NRF9160_HCI_CTS_PIN  25

/*
 * Send the startup No Operation Command Complete expected by the
 * Thingy:91 HCI UART host.
 */
#define HCI_SDC_STARTUP_NOP             1


#elif BOARD == WILDTHING91

#define BOARD_NAME                      "I-SYST WildThing91"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

/*
 * HCI is always UART. The nRF52840 UART pin map and hardware flow control are
 * the same as Thingy:91. The nRF91 reset line is NOT connected to nRF52840 on
 * WildThing91, so the reset-release early RTS gate is deliberately absent.
 * H:4 startup synchronization remains enabled to discard any nRF91 boot text
 * before the first HCI Reset.
 */
#define HCI_USB_SOCKET                  1
#define HCI_MODE_SWITCH                 0
#define HCI_STATUS_LEDS                 0
#define HCI_H4_STARTUP_RESET_SYNC       1

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_UART
#endif

#define UART_TX_PORT            0
#define UART_TX_PIN             25
#define UART_TX_PINOP           0

#define UART_RX_PORT            1
#define UART_RX_PIN             0
#define UART_RX_PINOP           0

#define UART_RTS_PORT           0
#define UART_RTS_PIN            19
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            22
#define UART_CTS_PINOP          0

#define UART_HW_FLOWCTRL	1
#define UART_DEVNO			0
#define UART_RATE			1000000


#elif BOARD == WILDTHING51

#define BOARD_NAME                      "I-SYST WildThing51"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

/*
 * HCI is always the BLYST840 <-> nRF9151 UART. WildThing51 does connect
 * BT_RST_CTRL to BT_nRESET, so it uses the reset-coupled early receive path.
 * The schematic gives the four interconnect nets as:
 *
 *     BTLTE0  nRF9151 P0.00  -> BLYST840 P0.23  (BLYST RX)
 *     BTLTE1  nRF9151 P0.01  -> BLYST840 P0.24  (BLYST TX)
 *     BTLTE2  nRF9151 P0.02  -> BLYST840 P0.21  (BLYST CTS)
 *     BTLTE3  nRF9151 P0.03  -> BLYST840 P1.04  (BLYST RTS)
 */
#define HCI_USB_SOCKET                  1
#define HCI_MODE_SWITCH                 0
#define HCI_STATUS_LEDS                 0
#define HCI_UART_EARLY_STARTUP          1
#define HCI_H4_STARTUP_RESET_SYNC       1

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_UART
#endif

#define UART_TX_PORT            0
#define UART_TX_PIN             24
#define UART_TX_PINOP           0

#define UART_RX_PORT            0
#define UART_RX_PIN             23
#define UART_RX_PINOP           0

#define UART_RTS_PORT           1
#define UART_RTS_PIN            4
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            21
#define UART_CTS_PINOP          0

#define UART_HW_FLOWCTRL	1
#define UART_DEVNO			0
#define UART_RATE			1000000


#else
#error "No pins defined. Define the pins used by your board."
#endif

//=============================================================================
// USB socket / board HCI policy defaults
//=============================================================================

#ifndef HCI_USB_SOCKET
#define HCI_USB_SOCKET		0
#endif

#ifndef HCI_MODE_SWITCH
#define HCI_MODE_SWITCH		0
#endif

#ifndef HCI_STATUS_LEDS
#define HCI_STATUS_LEDS		1
#endif

#ifndef HCI_UART_EARLY_STARTUP
#define HCI_UART_EARLY_STARTUP		0
#endif

#ifndef HCI_H4_STARTUP_RESET_SYNC
#define HCI_H4_STARTUP_RESET_SYNC	0
#endif

//=============================================================================
// UART flow control and pin map
//=============================================================================

/*
 * Built once for every board rather than copied into each branch, because the
 * two have to agree: asking the peripheral for hardware flow control without
 * RTS and CTS in the map gets a link that never sends, and putting them in the
 * map without asking for flow control drives two pins the peripheral will
 * never use.
 *
 * A board sets UART_HW_FLOWCTRL to 1 and defines the four RTS and CTS macros
 * to get both together.
 */
#ifndef UART_HW_FLOWCTRL
#define UART_HW_FLOWCTRL	0
#endif

#if UART_HW_FLOWCTRL

/*
 * A board that carries no RTS and CTS names would otherwise fail further down
 * on an undeclared macro inside the pin map, which does not say what to do.
 */
#if !defined(UART_RTS_PORT) || !defined(UART_CTS_PORT)
#error "UART_HW_FLOWCTRL needs UART_RTS_PORT/PIN/PINOP and UART_CTS_PORT/PIN/PINOP from the board"
#endif

#define UART_FLOWCTRL		UART_FLWCTRL_HW

#define UART_PINS			{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},}

#else

#define UART_FLOWCTRL		UART_FLWCTRL_NONE

#define UART_PINS			{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},}

#endif

//=============================================================================
// Board Initialization Function
//=============================================================================

#endif /* __BOARD_H__ */

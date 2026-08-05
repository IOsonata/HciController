/*
 * board.h
 *
 * IOsonata board pin assignments.
 *
 * This header intentionally contains ONLY pin assignments for devices used on the board.
 * Naming convention:
 *   <DEV_PIN_NAME>_PORT
 *   <DEV_PIN_NAME>_PIN
 *   <DEV_PIN_NAME>_PINOP   (MCU dependent; use 0 for GPIO; Nordic uses 0)
 */
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
 * Board ids 1 to 6 belong to IOsonata blyst840_boards.h. This one is local, so
 * it sits well clear of anything that header may add.
 */
#define THINGY91_NRF52840		100

#define BOARD			UDG_NRF52840
//#define BOARD			IBK_NRF52840
//#define BOARD			THINGY91_NRF52840

#if BOARD == UDG_NRF52840

#define BOARD_NAME                      "I-SYST UDG-NRF52840x Dongle"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

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

// Default pins for nRF52 (edit for your board)
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

#define UART_PINS			{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},}


#elif BOARD == IBK_NRF52840


#define BUTTON1_PINS					IBK_NRF52840_BUT_PINS_CFG

/* Product status LED channels, all active low. */

#define HCI_LED_RED_PORT                IBK_NRF52840_LED3_PORT
#define HCI_LED_RED_PIN                 IBK_NRF52840_LED3_PIN
#define HCI_LED_RED_ACTIVE              IBK_NRF52840_LED3_LOGIC

#define HCI_LED_GREEN_PORT              IBK_NRF52840_LED1_PORT
#define HCI_LED_GREEN_PIN               IBK_NRF52840_LED1_PIN
#define HCI_LED_GREEN_ACTIVE            IBK_NRF52840_LED1_LOGIC

#define HCI_LED_BLUE_PORT               IBK_NRF52840_LED2_PORT
#define HCI_LED_BLUE_PIN                IBK_NRF52840_LED2_PIN
#define HCI_LED_BLUE_ACTIVE             IBK_NRF52840_LED2_LOGIC

#define LED_PINS						IBK_NRF52840_LED_PINS_CFG

//=============================================================================
// UART Pin Definitions
//=============================================================================

// Default pins for nRF52 (edit for your board)
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

#define UART_PINS			{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},}


#elif BOARD == THINGY91_NRF52840

#define BOARD_NAME                      "Nordic Thingy:91"
#define BOARD_MODULE_NAME               "nRF52840"

/*
 * The nRF52840 on a Thingy:91 is the Bluetooth side of a pair. The nRF9160 is
 * the host and reaches it over the MCU_IF lines. Replacing the stock firmware
 * with this one takes the USB serial bridge away with it, since that is what
 * the stock firmware was doing.
 */

/*
 * No status LED. The RGB LED on a Thingy:91 hangs off the nRF9160, not this
 * part, so there is nothing here to drive and the pins the other boards use
 * are wired to something else entirely.
 */
#define HCI_STATUS_LEDS                 0

/*
 * The host is the nRF9160, always, and it is reached over UART. VBUS says
 * nothing about that here: the USB socket on a Thingy:91 goes to this part, so
 * leaving the choice to VBUS means a board on a charger comes up talking USB
 * to nobody while the nRF9160 waits for an answer that never comes.
 */
#define HCI_APP_FORCE_HOST              HCI_APP_HOST_UART

//=============================================================================
// UART Pin Definitions
//=============================================================================

/*
 * The nRF9160 side of these is its UART1: TX P0.22, RX P0.23, RTS P0.24 and
 * CTS P0.25. Crossed over, so this part's TX meets the nRF9160's RX.
 */
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

#define UART_DEVNO			0

#define UART_RATE			1000000

/* The nRF9160 side drives RTS and CTS, so this side has to honour them. */
#define UART_FLOWCTRL		UART_FLWCTRL_HW

#define UART_PINS			{ \
	{UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},\
	{UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL},}

/*
 * Low frequency clock. MPSL is told XTAL or RC from this, and it is the one
 * setting that stops the controller before it ever answers a command: told
 * XTAL on a board with no 32768 crystal, the clock never starts. RC always
 * works, at the cost of accuracy and a little current. Check the Thingy:91
 * hardware files, and if the crystal is there change the second entry to
 * {OSC_TYPE_XTAL, 32768, 20, 0} for the better clock.
 */
#define MCU_OSC			{ {OSC_TYPE_XTAL, 32000000, 20, 100}, {OSC_TYPE_RC, 32768, 250, 0}, false }

#else
#error "No pins defined. Define the pins used by your board."
#endif

//=============================================================================
// Board Initialization Function
//=============================================================================

#endif /* __BOARD_H__ */

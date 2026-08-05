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


#define BOARD			UDG_NRF52840
//#define BOARD			IBK_NRF52840

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

#define BUTTON1_PORT			UDG_NRF52840_BUT1_PORT
#define BUTTON1_PIN				UDG_NRF52840_BUT1_PIN
#define BUTTON1_PINOP			UDG_NRF52840_BUT1_PINOP

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


#else
#error "No board selected. Define BOARD to a supported board above."
#endif

//=============================================================================
// Board Initialization Function
//=============================================================================

#endif /* __BOARD_H__ */

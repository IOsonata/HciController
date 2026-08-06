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
 * The boards below are I-SYST hardware, and their ids come from IOsonata
 * blyst840_boards.h. A port to something else is a new id, a new branch in the
 * #if chain, and the pins and clock source that board actually has. Ids 1 to 6
 * are taken by that header, so a local one wants to sit well clear of them.
 */

/* -DBOARD=... on the command line wins, which is how the build is checked
 * against every board without editing this file. */
#ifndef BOARD
#define BOARD			UDG_NRF52840
//#define BOARD			IBK_NRF52840
#endif

//=============================================================================
// Host port selection
//=============================================================================

/*
 * Which port the controller talks to its host on, decided when the image is
 * built.
 *
 *   HCI_HOST_SELECT_AUTO   read VBUS at boot, powered is USB and otherwise UART
 *   HCI_HOST_SELECT_USB    always the USB CDC port
 *   HCI_HOST_SELECT_UART   always the UART
 *
 * Each board below picks its own, and -DHCI_HOST_SELECT=... on the command
 * line wins over the board, so a dongle can be built as a UART controller
 * without editing this file.
 *
 * AUTO only means something where the USB socket belongs to this part, which
 * is what a dongle is. Where the socket belongs to something else, VBUS reads
 * as a host that is not there: a board on a charger would come up talking USB
 * CDC to nobody while the real host waited for an answer over the UART. A
 * board whose host is another part on the same PCB names UART for that reason,
 * as the BLYST840 does when it answers to an nRF9151.
 *
 * Plain integers rather than an enum because main.cpp tests the selection with
 * #if, which does not see enumerators.
 *
 * None of them is zero, and that is deliberate. The preprocessor reads an
 * identifier it does not know as 0, so with an AUTO of 0 a misspelt
 * -DHCI_HOST_SELECT=HCI_HOST_SELECT_UARTT compiled clean and quietly gave the
 * VBUS reading image, overriding the board that had asked for UART. Starting
 * at 1 leaves 0 meaning nothing, so a misspelling matches no case at all and
 * main.cpp refuses it.
 */
#define HCI_HOST_SELECT_AUTO	1
#define HCI_HOST_SELECT_USB		2
#define HCI_HOST_SELECT_UART	3

#if BOARD == UDG_NRF52840

#define BOARD_NAME                      "I-SYST UDG-NRF52840x Dongle"
#define BOARD_MODULE_NAME               "I-SYST BLYST840"

/* The USB socket is this part's own, so VBUS is worth reading. */
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
 * Placeholders. This board reaches its host over USB, so nothing has ever put
 * a signal on these. They become real the moment the image is built with
 * -DHCI_HOST_SELECT=HCI_HOST_SELECT_UART, so check them against the schematic
 * before doing that.
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


#elif BOARD == IBK_NRF52840

/* The USB socket is this part's own, so VBUS is worth reading. */
#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT                 HCI_HOST_SELECT_AUTO
#endif

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

/*
 * Placeholders, as on the dongle above. Check them against the schematic
 * before building this board with -DHCI_HOST_SELECT=HCI_HOST_SELECT_UART.
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


#else
#error "No pins defined. Define the pins used by your board."
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

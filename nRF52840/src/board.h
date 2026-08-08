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

/*
 * Nordic Thingy:91. Not I-SYST hardware, so its id sits well clear of the
 * IOsonata range rather than pretending to belong to it. The nRF52840 on that
 * board reaches its host over the interconnect UART to the nRF9160, never over
 * USB, so it is the one board here where VBUS decides nothing about the host.
 * The socket is still this part's and is where the log goes.
 */
#define THINGY91_NRF52840	100

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
#define HCI_USB_SOCKET                  1

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
 * board's four and says which of them were measured rather than read.
 *
 * So these are named after the header the dongle brings out rather than after
 * another board's wiring, and they are still placeholders: this board reaches
 * its host over USB and nothing has put a signal on them. They become real the
 * moment the image is built with -DHCI_HOST_SELECT=HCI_HOST_SELECT_UART, so
 * check them against the schematic before doing that.
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

/* The USB socket is this part's own, so VBUS is worth reading. */
#define HCI_USB_SOCKET                  1

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
// UART Pin Definitions
//=============================================================================

/*
 * From sdk-nrf, boards/nordic/thingy91/thingy91_nrf52840-pinctrl.dtsi, the
 * uart1 node, which is the one wired to the nRF9160:
 *
 *     UART_TX  P0.25      UART_RX  P1.00
 *     UART_RTS P0.22      UART_CTS P0.19
 *
 * and the far side, thingy91_nrf9160_common-pinctrl.dtsi uart1:
 *
 *     UART_TX  P0.22      UART_RX  P0.23
 *     UART_RTS P0.24      UART_CTS P0.25
 *
 * Both board files say current-speed 1000000. TX and RX below are those two
 * and are confirmed on the hardware. RTS and CTS below are not, and the note
 * above them says what was measured instead.
 *
 * The other UART on each part is that part's own console, at 115200:
 *
 *     nRF52840 uart0   TX P0.15  RX P0.11  RTS P0.21  CTS P0.20
 *     nRF9160  uart0   TX P0.18  RX P0.19  RTS P0.20  CTS P0.21
 *
 * Written down because the two pairs are easy to confuse and picking the
 * wrong one gives a link with somebody's log on it at the wrong speed
 * rather than a link that fails. Both uart1 nodes ship status "disabled", so
 * an application that wants this interconnect enables it and chooses what
 * goes on it, and nothing in the board files can say what that turned out to
 * be.
 *
 * All four wires exist, so flow control is on. Off, this part never asserts
 * RTS, and a host that has flow control on never sees its CTS asserted and so
 * never transmits: the first command times out with nothing on the wire in
 * either direction, and no side can tell why.
 *
 * This link holds two things, not one, and that is the whole of what was wrong
 * with it for weeks. The nRF9160's bootloader prints on it before the
 * application ever opens it:
 *
 *     All pins have been configured as non-secure...
 *     <esc>[1;34mBooting TF-M...
 *
 * captured here off the wire. Then the application starts and uses the same
 * UART for HCI. The text is a prefix, not a permanent tenant, so the link does
 * become an HCI link, but this side is handed several hundred octets of it
 * first, on every reboot of the nRF9160.
 *
 * H:4 has no framing, so text that happens to hold an octet in the indicator
 * range makes this side read a payload length out of more text and wait for a
 * payload that never comes. The HCI Reset behind it is eaten as that payload.
 * Nothing in the octets says so, and the host times out after ten seconds with
 * a link that looks correct from both ends.
 *
 * The gap between the banner and the first command is the only thing that
 * separates them: the banner is printed before Zephyr starts and the Reset
 * arrives about a hundred milliseconds into the application. So the transport
 * gives up a half built packet once the link has been quiet, which lands in
 * that gap and takes the Reset cleanly. See HCI_APP_LINK_IDLE_PASSES.
 *
 * Where the host can be configured, Zephyr's CONFIG_BT_WAIT_NOP=y holds its
 * command semaphore at zero until a Command Complete for the No Operation
 * opcode arrives, which is what HciSdcNrfxlibQueueStartupNop queues here.
 * That would make the start up a handshake rather than a race. A proprietary
 * host cannot be told to do it, so it is written down as something available
 * rather than as something in use.
 *
 * What has been checked, against sdk-nrf and the Thingy:91 hardware guide:
 * the four pins above and their rate, that the low frequency crystal is on
 * P0.00 and P0.01 so the default LFXO clock configuration is right for this
 * board, and that the commands a Zephyr host sends during bt_enable are all
 * dispatched here.
 */
#define UART_TX_PORT            0
#define UART_TX_PIN             25
#define UART_TX_PINOP           0

#define UART_RX_PORT            1
#define UART_RX_PIN             0
#define UART_RX_PINOP           0

/*
 * RTS is P0.19 and CTS is P0.22, which is the opposite of what the sdk-nrf
 * pinctrl above assigns, and the hardware is what says so:
 *
 *     RTS on P0.19   the peer transmits, thousands of octets a second
 *     RTS on P0.22   the peer transmits nothing at all
 *
 * Measured on the board, both ways round, more than once. RTS is what tells
 * the peer it may send, so driving the wrong pin leaves the peer's clear to
 * send never asserted and the peer silent. That is what P0.22 gives. TX and RX
 * are not affected and stay as the pinctrl has them.
 *
 * Why a Nordic board file would disagree about its own peripheral assignment
 * is not resolved here, and guessing at it is what caused the damage. This
 * pair has now been changed three times on reasoning rather than measurement,
 * twice into a state that does not work, and each time the reasoning sounded
 * good. The measurement is short, repeatable and was available throughout.
 * Nothing about this pair gets changed again without one.
 *
 * The reason the argument could run at all is that the pair being wrong is
 * quiet in the other direction. Where this part drives RTS onto a line the
 * peer also drives, two outputs fight; where it reads CTS from a line nothing
 * drives, the line floats and usually reads as permission to send. Octets can
 * still move, nothing reports an error, and the flow control does nothing.
 *
 * So the firmware measures it at start up too. Each pin is held with a pull up
 * and then a pull down before the UART takes it: a pin whose level follows the
 * pull has nothing driving it from the far side and is this part's output, and
 * a pin that ignores the pull is being driven and is this part's input. The
 * answer is in the log as "flow:", on every boot, without anyone having to set
 * up an experiment for it.
 */
#define UART_RTS_PORT           0
#define UART_RTS_PIN            19
#define UART_RTS_PINOP          0

#define UART_CTS_PORT           0
#define UART_CTS_PIN            22
#define UART_CTS_PINOP          0

#define UART_HW_FLOWCTRL	1

#define UART_DEVNO			0

#define UART_RATE			1000000


#else
#error "No pins defined. Define the pins used by your board."
#endif

//=============================================================================
// USB socket
//=============================================================================

/*
 * Whether the USB socket on the board is wired to this part.
 *
 * A separate question from which port the HCI stream is on, and the Thingy:91
 * is why: the socket there is the nRF52840's and the host is the nRF9160 on
 * the other end of the interconnect UART. The socket has no HCI stream on it
 * and is where the log goes instead.
 *
 * A board that says nothing gets 0, and a UART host on it brings no USB up.
 * That is the right way round: where the socket belongs to another part,
 * enabling the peripheral would put a device on a bus that is not this one's
 * to enumerate on, and the cost of the default being wrong the other way is
 * only a log nobody reads.
 */
#ifndef HCI_USB_SOCKET
#define HCI_USB_SOCKET		0
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

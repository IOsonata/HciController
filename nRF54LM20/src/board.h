/**-------------------------------------------------------------------------
@file board.h
@brief nRF54LM20 HciController board/transport policy.
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#ifndef __BOARD_H__
#define __BOARD_H__

/* These IDs are local to the nRF54LM20 target; never reuse nRF52 GPIO maps. */
#define UDG_NRF54LM20 200
#define NORDIC_DK_NRF54LM20 201
#define WILDTHING51_NRF54LM20 101

#ifndef BOARD
#define BOARD UDG_NRF54LM20
#endif

#define HCI_HOST_SELECT_AUTO 1
#define HCI_HOST_SELECT_USB 2
#define HCI_HOST_SELECT_UART 3

#if BOARD == UDG_NRF54LM20

#define BOARD_NAME "I-SYST UDG-NRF54LM20"
#define BOARD_MODULE_NAME "Nordic nRF54LM20"
#define HCI_USB_SOCKET 1
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 0
#define HCI_UART_EARLY_STARTUP 0
#define HCI_BOARD_HAS_UART 0
#define HCI_HOST_SELECT HCI_HOST_SELECT_USB

/* USB HCI plus the existing local diagnostic CDC; no UART hardware. */

#elif BOARD == NORDIC_DK_NRF54LM20

#define BOARD_NAME "Nordic nRF54LM20 DK"
#define BOARD_MODULE_NAME "Nordic nRF54LM20"
#define HCI_USB_SOCKET 1
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 1
#define HCI_UART_EARLY_STARTUP 0
#define HCI_BOARD_HAS_UART 1

#define HCI_LED_RED_PORT 1
#define HCI_LED_RED_PIN 22
#define HCI_LED_RED_ACTIVE LED_LOGIC_HIGH
#define HCI_LED_GREEN_PORT 1
#define HCI_LED_GREEN_PIN 25
#define HCI_LED_GREEN_ACTIVE LED_LOGIC_HIGH
#define HCI_LED_BLUE_PORT 1
#define HCI_LED_BLUE_PIN 27
#define HCI_LED_BLUE_ACTIVE LED_LOGIC_HIGH
#define LED_PINS { \
    {1, 22, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 25, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 27, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT HCI_HOST_SELECT_USB
#endif

/* DK UARTE20, from IOsonata nRF54LM20x DK board.h. */
#define UART_DEVNO 1
#define UART_RX_PORT 1
#define UART_RX_PIN 17
#define UART_RX_PINOP 1
#define UART_TX_PORT 1
#define UART_TX_PIN 16
#define UART_TX_PINOP 1
#define UART_CTS_PORT 1
#define UART_CTS_PIN 19
#define UART_CTS_PINOP 1
#define UART_RTS_PORT 1
#define UART_RTS_PIN 18
#define UART_RTS_PINOP 1
#define UART_RATE 1000000
#define UART_HW_FLOWCTRL 1
#define UART_FLOWCTRL UART_FLWCTRL_HW

#elif BOARD == WILDTHING51_NRF54LM20

#define BOARD_NAME "I-SYST WildThing51 (nRF54LM20)"
#define BOARD_MODULE_NAME "Nordic nRF54LM20"
#define HCI_USB_SOCKET 1
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 0
#define HCI_UART_EARLY_STARTUP 0
#define HCI_H4_STARTUP_RESET_SYNC 1
#define HCI_BOARD_HAS_UART 1
#define HCI_HOST_SELECT HCI_HOST_SELECT_UART

/*
 * WildThing51 BLE footprint M2 is pin-compatible at board level with
 * either BLYST840/nRF52840 or nRF54LM20. Each net has two pin labels
 * separated by '/', the second belonging to nRF54LM20 (BLE sheet 3).
 *
 * Net       nRF9151      nRF52840      nRF54LM20      HCI direction
 * BTLTE0    P0.00        P0.23         P1.14          BLE RX
 * BTLTE1    P0.01        P0.24         P3.04          BLE TX
 * BTLTE2    P0.02        P0.21         P3.05          BLE CTS
 * BTLTE3    P0.03        P1.04         P1.13          BLE RTS
 *
 * The four BTLTE nets are all allocated to HCI UART (1 Mbaud, RTS/CTS).
 * The supplied schematic does NOT route an additional nRF91 debug UART
 * TX/RX to this BLE footprint. An independent nRF91 trace UART therefore
 * requires confirmed additional wiring; do not guess or borrow HCI pins.
 */
#define UART_RX_PORT 1
#define UART_RX_PIN 14
#define UART_TX_PORT 3
#define UART_TX_PIN 4
#define UART_CTS_PORT 3
#define UART_CTS_PIN 5
#define UART_RTS_PORT 1
#define UART_RTS_PIN 13

/* The selected IOsonata UARTE instance must support these P1/P3 pin routes. */
#if !defined(WILDTHING51_HCI_UART_DEVNO)
#error "Select the nRF54LM20 UARTE instance for WildThing51 HCI"
#endif
#if !defined(WILDTHING51_TRACE_UART_DEVNO) || \
    !defined(WILDTHING51_TRACE_RX_PORT) || !defined(WILDTHING51_TRACE_RX_PIN) || \
    !defined(WILDTHING51_TRACE_TX_PORT) || !defined(WILDTHING51_TRACE_TX_PIN)
#error "WildThing51 schematic has no separate nRF91 trace RX/TX interconnect; supply verified wiring"
#endif

#if WILDTHING51_HCI_UART_DEVNO == WILDTHING51_TRACE_UART_DEVNO
#error "WildThing51 HCI and trace UARTs must be different peripherals"
#endif

#define UART_DEVNO WILDTHING51_HCI_UART_DEVNO
#define UART_RATE 1000000
#define UART_RX_PINOP 1
#define UART_TX_PINOP 1
#define UART_RTS_PINOP 1
#define UART_CTS_PINOP 1
#define UART_HW_FLOWCTRL 1
#define UART_FLOWCTRL UART_FLWCTRL_HW

#define HCI_NRF91_TRACE_BRIDGE 1
#define HCI_NRF91_TRACE_UART_DEVNO WILDTHING51_TRACE_UART_DEVNO
#define HCI_NRF91_TRACE_UART_RATE 115200
#define HCI_NRF91_TRACE_UART_PINS { \
    {WILDTHING51_TRACE_RX_PORT, WILDTHING51_TRACE_RX_PIN, 1, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {WILDTHING51_TRACE_TX_PORT, WILDTHING51_TRACE_TX_PIN, 1, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}

#else
#error "Unsupported nRF54LM20 board"
#endif

#if HCI_BOARD_HAS_UART
#define UART_PINS { \
    {UART_RX_PORT, UART_RX_PIN, UART_RX_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_TX_PORT, UART_TX_PIN, UART_TX_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_CTS_PORT, UART_CTS_PIN, UART_CTS_PINOP, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {UART_RTS_PORT, UART_RTS_PIN, UART_RTS_PINOP, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}
#endif

#endif /* __BOARD_H__ */

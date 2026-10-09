/**-------------------------------------------------------------------------
@file board.h

@brief nRF54LM20 DK (PCA10184) HciController board configuration.

Board wiring is application policy. The nRF54LM20 controller implementation
must not contain DK pin numbers.

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#ifndef __BOARD_H__
#define __BOARD_H__

#ifndef BOARD
#define BOARD 200
#endif
#define BOARD_NAME "Nordic nRF54LM20 DK"
#define BOARD_MODULE_NAME "Nordic nRF54LM20A"

/* The DK USB socket is owned by the nRF54LM20. */
#define HCI_USB_SOCKET 1
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 1
/* DK LEDs 0,1,2; each is a separate active-high mono LED. */
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
#define HCI_UART_EARLY_STARTUP 0

#define HCI_HOST_SELECT_AUTO 1
#define HCI_HOST_SELECT_USB 2
#define HCI_HOST_SELECT_UART 3
#ifndef HCI_HOST_SELECT
#define HCI_HOST_SELECT HCI_HOST_SELECT_USB
#endif

/* Virtual COM port 1: UARTE20, P1.16/17, P1.18/19.
 * Pin numbers match IOsonata's nRF54LM20x BleAdvertiser DK board.h.
 * A different board must provide its own mapping.
 */
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
#define UART_PINS { \
    {1, 17, 1, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 16, 1, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 19, 1, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 18, 1, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}
#define NRFX_UART_INST 20
#define UART_HW_FLOWCTRL 1
#define UART_FLOWCTRL UART_FLWCTRL_HW

#endif /* __BOARD_H__ */

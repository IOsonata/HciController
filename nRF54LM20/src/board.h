/**-------------------------------------------------------------------------
@file board.h

@brief nRF54LM20 DK (PCA10184) HciController board configuration.

Board wiring is application policy. The nRF54LM20 controller implementation
must not contain DK pin numbers.

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#ifndef __BOARD_H__
#define __BOARD_H__

#define BOARD 200
#define BOARD_NAME "Nordic nRF54LM20 DK"
#define BOARD_MODULE_NAME "Nordic nRF54LM20A"

/* The DK USB socket is owned by the nRF54LM20. */
#define HCI_USB_SOCKET 1
#define HCI_MODE_SWITCH 0
#define HCI_STATUS_LEDS 0
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
#define UART_RATE 1000000
#define UART_PINS { \
    {1, 17, 1, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 16, 1, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 18, 1, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
    {1, 19, 1, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL}, \
}
#define UART_HW_FLOWCTRL 1

#endif /* __BOARD_H__ */

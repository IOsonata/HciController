/*
 * A stand in board for the host build.
 *
 * The real board.h lives with the project that builds the firmware, not in
 * this repository, and it is what says which UART instance the host is on,
 * which pins it uses and at what rate. This one describes a board with flow
 * control on all four pins, which is the case that exercises the most of
 * src/hci_app.cpp: the pin array, the instance number and the flow control
 * setting all come from here.
 *
 * The pin numbers are made up. Nothing here reaches a peripheral.
 */

#ifndef BOARD_H__
#define BOARD_H__

#include "coredev/iopincfg.h"
#include "coredev/uart.h"

#define UART_DEVNO 0

#define UART_RATE 1000000

#define UART_FLOWCTRL UART_FLWCTRL_HW

#define UART_RX_PORT  0
#define UART_RX_PIN   8
#define UART_TX_PORT  0
#define UART_TX_PIN   6
#define UART_CTS_PORT 0
#define UART_CTS_PIN  22
#define UART_RTS_PORT 0
#define UART_RTS_PIN  19

/* No reset-coupled startup behavior in the host compile fixture. */
#define HCI_UART_EARLY_STARTUP       0
#define HCI_H4_STARTUP_RESET_SYNC    0

#define UART_PINS                                                             \
    {                                                                         \
        {UART_RX_PORT, UART_RX_PIN, 1, IOPINDIR_INPUT, IOPINRES_NONE,         \
         IOPINTYPE_NORMAL},                                                   \
        {UART_TX_PORT, UART_TX_PIN, 1, IOPINDIR_OUTPUT, IOPINRES_NONE,        \
         IOPINTYPE_NORMAL},                                                   \
        {UART_CTS_PORT, UART_CTS_PIN, 1, IOPINDIR_INPUT, IOPINRES_NONE,       \
         IOPINTYPE_NORMAL},                                                   \
        {UART_RTS_PORT, UART_RTS_PIN, 1, IOPINDIR_OUTPUT, IOPINRES_NONE,      \
         IOPINTYPE_NORMAL},                                                   \
    }

#endif /* BOARD_H__ */

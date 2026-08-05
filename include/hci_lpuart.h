/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_LPUART_H
#define HCI_LPUART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The handshake Nordic runs over a UART between two chips that both want their
 * receivers switched off between transfers, nrf_sw_lpuart. An nRF9160 paired
 * with an nRF52840 uses it, so a controller that speaks only H:4 over a plain
 * UART gets no answer from such a host: the host never enables its receiver,
 * and never sends, because nothing acknowledged its request.
 *
 * Two wires, crossed, one per direction. This side's req output goes to the
 * other side's rdy input, and its req comes back to this side's rdy. Each wire
 * is driven by whichever end is sending on it, released to a pull up while the
 * other end answers, and driven again for the length of the transfer.
 *
 * Sending:
 *
 *   req driven low            idle
 *   req driven high           the request, which the peer sees as a level
 *   req released, pull up     the pull up holds the line while the peer answers
 *   peer pulls the line low   acknowledged, and its receiver is now on
 *   req driven high           held for the whole transfer
 *   bytes go out
 *   req driven low            the end, which tells the peer to stop receiving
 *
 * Receiving is the same sequence read from the other end. A level on rdy is a
 * request, and it is answered by driving rdy low and releasing it, but only
 * after this side is ready for the bytes. The falling edge that follows the
 * transfer says the peer has finished.
 *
 * Answering the request is what interoperability needs. Switching the receiver
 * off in between is what saves the current, and a port that leaves its receiver
 * on still talks to the peer correctly, so RxEnable may do nothing.
 *
 * None of this touches a register. The port supplies the pin and UART
 * operations, which keeps the sequence testable off target, where every
 * ordering below is asserted.
 */

typedef enum {
    HCI_LPUART_TX_IDLE = 0,
    HCI_LPUART_TX_WAIT_ACK,   /* req released, waiting for the peer to pull */
    HCI_LPUART_TX_ACTIVE,     /* req driven high, bytes going out */
} HciLpUartTxState_t;

typedef enum {
    HCI_LPUART_RX_IDLE = 0,   /* rdy watched for a level */
    HCI_LPUART_RX_ACTIVE,     /* acknowledged, rdy watched for the end */
} HciLpUartRxState_t;

/*
 * What a wire can be asked to do. Drive puts a level on it. Release makes it an
 * input with a pull up and arms the fall that the peer causes. Watch arms
 * either the level that starts a transfer or the fall that ends one.
 */
typedef void (*HciLpUartPinDrive_t)(void *pContext, bool High);
typedef void (*HciLpUartPinRelease_t)(void *pContext);
typedef void (*HciLpUartPinWatch_t)(void *pContext, bool Level);
typedef bool (*HciLpUartSend_t)(void *pContext, const uint8_t *pData, size_t Len);
typedef void (*HciLpUartRxEnable_t)(void *pContext, bool Enable);
typedef uint32_t (*HciLpUartMs_t)(void *pContext);

typedef struct {
    HciLpUartPinDrive_t ReqDrive;
    HciLpUartPinRelease_t ReqRelease;
    HciLpUartPinDrive_t RdyDrive;
    HciLpUartPinRelease_t RdyRelease;
    HciLpUartPinWatch_t RdyWatch;
    HciLpUartSend_t Send;
    HciLpUartRxEnable_t RxEnable;   /* may be NULL where the receiver stays on */
    HciLpUartMs_t Ms;
    void *pContext;

    /*
     * How long to wait for the peer to acknowledge a request. Nordic's driver
     * carries the same idea as a Kconfig default. Zero takes the default below,
     * since a timeout of zero would abort every transfer before it started.
     */
    uint32_t AckTimeoutMs;
} HciLpUartOps_t;

#define HCI_LPUART_DEFAULT_ACK_TIMEOUT_MS 10U

typedef struct {
    HciLpUartOps_t Ops;

    HciLpUartTxState_t TxState;
    HciLpUartRxState_t RxState;

    const uint8_t *pTxData;
    size_t TxLen;
    uint32_t TxStartedMs;

    uint32_t AckTimeoutCount;
    uint32_t SendErrorCount;
    uint32_t RequestCount;
    uint32_t UnexpectedEventCount;
} HciLpUart_t;

bool HciLpUartInit(HciLpUart_t *pLp, const HciLpUartOps_t *pOps);

/*
 * Start sending. The bytes are not copied, so they have to stay put until the
 * port reports the transfer done. False means a transfer is already running or
 * the arguments are unusable.
 */
bool HciLpUartSend(HciLpUart_t *pLp, const uint8_t *pData, size_t Len);

bool HciLpUartSendBusy(const HciLpUart_t *pLp);

/* The peer pulled the request wire low, so it is ready for the bytes. */
void HciLpUartReqFell(HciLpUart_t *pLp);

/* The port finished putting the bytes on the wire. */
void HciLpUartSendDone(HciLpUart_t *pLp);

/*
 * The watched change happened on rdy. Level true is the peer asking to send,
 * false is the fall that says it has finished.
 */
void HciLpUartRdyEvent(HciLpUart_t *pLp, bool Level);

/*
 * Call regularly. Only the acknowledge timeout needs it, and without it a peer
 * that never answers leaves this side unable to send again.
 */
void HciLpUartProcess(HciLpUart_t *pLp);

#ifdef __cplusplus
}
#endif

#endif /* HCI_LPUART_H */

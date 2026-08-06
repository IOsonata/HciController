/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_lpuart.h"

#include <string.h>

static void HciLpUartRxEnable(HciLpUart_t *pLp, bool Enable)
{
    if (pLp->Ops.RxEnable != NULL)
    {
        pLp->Ops.RxEnable(pLp->Ops.pContext, Enable);
    }
}

/*
 * Back to idle on the sending side: the wire driven low, which is both the
 * resting level and what tells a peer mid transfer that the bytes have ended.
 */
static void HciLpUartTxIdle(HciLpUart_t *pLp)
{
    pLp->TxState = HCI_LPUART_TX_IDLE;
    pLp->pTxData = NULL;
    pLp->TxLen = 0U;
    pLp->Ops.ReqDrive(pLp->Ops.pContext, false);
}

bool HciLpUartInit(HciLpUart_t *pLp, const HciLpUartOps_t *pOps)
{
    if (pLp == NULL || pOps == NULL ||
        pOps->ReqDrive == NULL || pOps->ReqRelease == NULL ||
        pOps->RdyDrive == NULL || pOps->RdyRelease == NULL ||
        pOps->RdyWatch == NULL || pOps->Send == NULL || pOps->Ms == NULL)
    {
        return false;
    }

    memset(pLp, 0, sizeof(*pLp));
    pLp->Ops = *pOps;
    if (pLp->Ops.AckTimeoutMs == 0U)
    {
        pLp->Ops.AckTimeoutMs = HCI_LPUART_DEFAULT_ACK_TIMEOUT_MS;
    }

    /*
     * Both wires resting before either side is watched, so a peer that is
     * already up does not read a request out of a pin that has not been
     * driven yet.
     */
    pLp->TxState = HCI_LPUART_TX_IDLE;
    pLp->Ops.ReqDrive(pLp->Ops.pContext, false);

    pLp->RxState = HCI_LPUART_RX_IDLE;
    HciLpUartRxEnable(pLp, false);
    pLp->Ops.RdyRelease(pLp->Ops.pContext);
    pLp->Ops.RdyWatch(pLp->Ops.pContext, true);

    return true;
}

bool HciLpUartSendBusy(const HciLpUart_t *pLp)
{
    return pLp != NULL && pLp->TxState != HCI_LPUART_TX_IDLE;
}

bool HciLpUartSend(HciLpUart_t *pLp, const uint8_t *pData, size_t Len)
{
    if (pLp == NULL || pData == NULL || Len == 0U)
    {
        return false;
    }

    if (pLp->TxState != HCI_LPUART_TX_IDLE)
    {
        return false;
    }

    pLp->pTxData = pData;
    pLp->TxLen = Len;
    pLp->TxStartedMs = pLp->Ops.Ms(pLp->Ops.pContext);
    pLp->TxState = HCI_LPUART_TX_WAIT_ACK;

    /*
     * Drive the request, then let go. The peer reads a level rather than an
     * edge, so the pull up has to hold the wire high through the gap between
     * letting go and the peer answering, and the peer answers by pulling it
     * low against that pull up.
     */
    pLp->Ops.ReqDrive(pLp->Ops.pContext, true);
    pLp->Ops.ReqRelease(pLp->Ops.pContext);
    return true;
}

void HciLpUartReqFell(HciLpUart_t *pLp)
{
    if (pLp == NULL)
    {
        return;
    }

    if (pLp->TxState != HCI_LPUART_TX_WAIT_ACK)
    {
        /*
         * A fall with nothing waiting for it. The wire is shared with the
         * peer's idea of when a transfer ends, so this is noise rather than a
         * fault, and answering it by sending would put bytes on a wire nobody
         * is listening to.
         */
        pLp->UnexpectedEventCount++;
        return;
    }

    /* Acknowledged. Hold the wire high for as long as the bytes take. */
    pLp->Ops.ReqDrive(pLp->Ops.pContext, true);
    pLp->TxState = HCI_LPUART_TX_ACTIVE;

    if (!pLp->Ops.Send(pLp->Ops.pContext, pLp->pTxData, pLp->TxLen))
    {
        pLp->SendErrorCount++;
        HciLpUartTxIdle(pLp);
    }
}

void HciLpUartSendDone(HciLpUart_t *pLp)
{
    if (pLp == NULL)
    {
        return;
    }

    if (pLp->TxState != HCI_LPUART_TX_ACTIVE)
    {
        pLp->UnexpectedEventCount++;
        return;
    }

    HciLpUartTxIdle(pLp);
}

void HciLpUartRdyEvent(HciLpUart_t *pLp, bool Level)
{
    if (pLp == NULL)
    {
        return;
    }

    if (Level)
    {
        if (pLp->RxState != HCI_LPUART_RX_IDLE)
        {
            pLp->UnexpectedEventCount++;
            return;
        }

        /*
         * Ready first, acknowledge second. The peer starts sending the moment
         * it sees the wire fall, so anything this side needs doing has to be
         * done before the wire is touched or the first bytes are lost.
         */
        HciLpUartRxEnable(pLp, true);
        pLp->Ops.RdyDrive(pLp->Ops.pContext, false);
        pLp->Ops.RdyRelease(pLp->Ops.pContext);

        pLp->RxState = HCI_LPUART_RX_ACTIVE;
        pLp->RequestCount++;

        /* From here the fall at the end of the transfer is what matters. */
        pLp->Ops.RdyWatch(pLp->Ops.pContext, false);
        return;
    }

    if (pLp->RxState != HCI_LPUART_RX_ACTIVE)
    {
        pLp->UnexpectedEventCount++;
        /* Put the watch back where it belongs rather than leaving it armed. */
        pLp->Ops.RdyWatch(pLp->Ops.pContext, true);
        return;
    }

    HciLpUartRxEnable(pLp, false);
    pLp->RxState = HCI_LPUART_RX_IDLE;
    pLp->Ops.RdyWatch(pLp->Ops.pContext, true);
}

void HciLpUartProcess(HciLpUart_t *pLp)
{
    if (pLp == NULL || pLp->TxState != HCI_LPUART_TX_WAIT_ACK)
    {
        return;
    }

    const uint32_t now = pLp->Ops.Ms(pLp->Ops.pContext);
    if ((uint32_t)(now - pLp->TxStartedMs) < pLp->Ops.AckTimeoutMs)
    {
        return;
    }

    /*
     * Read the state again before acting on it. Reading the clock is not
     * instant, and the acknowledge arrives from the pin interrupt, so between
     * the test above and here the peer can have answered and the port can
     * already be putting bytes on the wire. Aborting then would drive the
     * request low in the middle of a transfer, which the peer reads as the end
     * of a packet it has only half received, and would release a buffer the
     * port is still sending from.
     *
     * This narrows the window rather than closing it. Closing it needs the
     * request interrupt masked across the whole function, which only the port
     * can do, so a port that calls this from thread context should mask it.
     */
    if (pLp->TxState != HCI_LPUART_TX_WAIT_ACK)
    {
        return;
    }

    /*
     * The peer never answered. Give the wire back rather than hold a request
     * up forever, since a peer that comes back later has to see a fresh one,
     * and this side has to be able to send again.
     */
    pLp->AckTimeoutCount++;
    HciLpUartTxIdle(pLp);
}

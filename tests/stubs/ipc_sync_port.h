/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * Synchronous IpcIntrf port for host tests. Raising a channel runs the
 * receive work of the other core at once, in the same thread. The channel
 * number is both the send and the receive channel of the pair, as in the
 * nRF5340 port. Include it in one source file of a test program: it defines
 * the three port functions.
 */
#ifndef STUB_IPC_SYNC_PORT_H
#define STUB_IPC_SYNC_PORT_H

#include "ipc/ipc_intrf.h"

static IpcIntrfDev_t *s_pIpcByRx[16];

extern "C" bool IpcCtrlrInit(IpcIntrfDev_t * const pDev)
{
    s_pIpcByRx[pDev->RxChan] = pDev;
    return true;
}

extern "C" void IpcCtrlrSend(int Chan)
{
    if (s_pIpcByRx[Chan] != nullptr)
    {
        IpcIntrfIrqHandler(s_pIpcByRx[Chan]);
    }
}

extern "C" void IpcCtrlrRxEnable(IpcIntrfDev_t * const, bool)
{
}

#endif

/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * A controller for the bridge tests: the packet DeviceIntrf the bridge
 * drives, over put/get callbacks a test supplies. StartRx(type) fetches
 * the next packet into a hold and says whether it is of that type, the
 * way the SDC face in hci_sdc.cpp does. A get error or a packet that is
 * not host facing is dropped and counted here, as the SDC counts its own.
 */
#ifndef HCI_CTLR_FAKE_INTRF_H
#define HCI_CTLR_FAKE_INTRF_H

#ifndef HCI_CTLR_FAKE_HOLD_SIZE
#define HCI_CTLR_FAKE_HOLD_SIZE 1024U
#endif

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_intrf.h"
#include "hci_h4.h"
#include "hci_sdc.h"

typedef bool (*HciCtlrFakePut_t)(void *pContext, HciH4PacketType_t Type,
                                 const uint8_t *pPacket, size_t PacketLen);
typedef HciSdcGetResult_t (*HciCtlrFakeGet_t)(void *pContext,
                                              HciH4PacketType_t *pType,
                                              uint8_t *pPacket,
                                              size_t PacketCapacity,
                                              size_t *pPacketLen);

typedef struct
{
    DevIntrf_t Intrf;
    HciCtlrFakePut_t Put;
    HciCtlrFakeGet_t Get;
    void *pContext;
    uint8_t Held[HCI_CTLR_FAKE_HOLD_SIZE];
    size_t HeldLen;
    HciH4PacketType_t HeldType;
    HciH4PacketType_t TxType;
    unsigned GetErrorCount;
    unsigned DroppedCount;
} HciCtlrFakeIntrf_t;

static inline HciCtlrFakeIntrf_t *HciCtlrFakeOwner(DevIntrf_t * const p)
{
    return (HciCtlrFakeIntrf_t *)p->pDevData;
}

static inline void HciCtlrFakeNop(DevIntrf_t * const) {}
static inline uint32_t HciCtlrFakeRate(DevIntrf_t * const) { return 0U; }
static inline uint32_t HciCtlrFakeSetRate(DevIntrf_t * const, uint32_t) { return 0U; }
static inline void *HciCtlrFakeHandle(DevIntrf_t * const p) { return HciCtlrFakeOwner(p); }

static inline bool HciCtlrFakeStartRx(DevIntrf_t * const p, uint32_t DevAddr)
{
    HciCtlrFakeIntrf_t *f = HciCtlrFakeOwner(p);

    if (f->HeldLen == 0U)
    {
        HciH4PacketType_t type = HCI_H4_PACKET_NONE;
        size_t len = 0U;
        HciSdcGetResult_t r = f->Get(f->pContext, &type, f->Held, sizeof(f->Held), &len);
        if (r == HCI_SDC_GET_EMPTY)
        {
            return false;
        }
        if (r != HCI_SDC_GET_PACKET)
        {
            f->GetErrorCount++;
            return false;
        }
        if (len == 0U || (type != HCI_H4_PACKET_EVENT && type != HCI_H4_PACKET_ACL &&
                          type != HCI_H4_PACKET_ISO && type != HCI_H4_PACKET_SCO))
        {
            f->DroppedCount++;
            return false;
        }
        f->HeldType = type;
        f->HeldLen = len;
    }
    return (uint32_t)f->HeldType == DevAddr;
}

static inline int HciCtlrFakeRxData(DevIntrf_t * const p, uint8_t *pBuffer, int BufferLen)
{
    HciCtlrFakeIntrf_t *f = HciCtlrFakeOwner(p);

    if (f->HeldLen == 0U || BufferLen < (int)f->HeldLen)
    {
        return 0;
    }
    int l = (int)f->HeldLen;
    memcpy(pBuffer, f->Held, f->HeldLen);
    f->HeldLen = 0U;
    return l;
}

static inline bool HciCtlrFakeStartTx(DevIntrf_t * const p, uint32_t DevAddr)
{
    HciCtlrFakeOwner(p)->TxType = (HciH4PacketType_t)DevAddr;
    return true;
}

static inline int HciCtlrFakeTxData(DevIntrf_t * const p, const uint8_t *pData, int DataLen)
{
    HciCtlrFakeIntrf_t *f = HciCtlrFakeOwner(p);
    return f->Put(f->pContext, f->TxType, pData, (size_t)DataLen) ? DataLen : 0;
}

static inline void HciCtlrFakeReset(DevIntrf_t * const p)
{
    HciCtlrFakeIntrf_t *f = HciCtlrFakeOwner(p);
    f->HeldLen = 0U;
}

static inline DevIntrf_t *HciCtlrFakeIntrfInit(HciCtlrFakeIntrf_t *f,
                                               HciCtlrFakePut_t Put,
                                               HciCtlrFakeGet_t Get,
                                               void *pContext)
{
    memset(f, 0, sizeof(*f));
    f->Put = Put;
    f->Get = Get;
    f->pContext = pContext;

    DevIntrf_t *d = &f->Intrf;
    d->pDevData = f;
    d->Type = DEVINTRF_TYPE_BT;
    d->MaxRetry = 1;
    atomic_flag_clear(&d->bBusy);
    atomic_store(&d->EnCnt, 0);
    atomic_store(&d->bTxReady, true);
    atomic_store(&d->bNoStop, false);
    d->Disable = HciCtlrFakeNop;
    d->Enable = HciCtlrFakeNop;
    d->GetRate = HciCtlrFakeRate;
    d->SetRate = HciCtlrFakeSetRate;
    d->StartRx = HciCtlrFakeStartRx;
    d->RxData = HciCtlrFakeRxData;
    d->StopRx = HciCtlrFakeNop;
    d->StartTx = HciCtlrFakeStartTx;
    d->TxData = HciCtlrFakeTxData;
    d->TxSrData = HciCtlrFakeTxData;
    d->StopTx = HciCtlrFakeNop;
    d->Reset = HciCtlrFakeReset;
    d->PowerOff = HciCtlrFakeNop;
    d->GetHandle = HciCtlrFakeHandle;
    return d;
}

/*
 * The bridge tests stand in for DeviceIntrfRx and DeviceIntrfTx to script
 * the host side. These run the fake controller through its own hooks the
 * way the IOsonata sequence does, for the device that is not the host.
 */
static inline int HciCtlrFakeRx(DevIntrf_t * const pDev, uint32_t DevAddr,
                                uint8_t *pBuffer, int BufferLen)
{
    if (!DeviceIntrfStartRx(pDev, DevAddr))
    {
        return 0;
    }
    const int result = DeviceIntrfRxData(pDev, pBuffer, BufferLen);
    DeviceIntrfStopRx(pDev);
    return result;
}

static inline int HciCtlrFakeTx(DevIntrf_t * const pDev, uint32_t DevAddr,
                                const uint8_t *pData, int DataLen)
{
    if (!DeviceIntrfStartTx(pDev, DevAddr))
    {
        return 0;
    }
    const int result = DeviceIntrfTxData(pDev, pData, DataLen);
    DeviceIntrfStopTx(pDev);
    return result;
}

#endif

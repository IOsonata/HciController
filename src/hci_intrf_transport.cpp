/**-------------------------------------------------------------------------
@file	hci_intrf_transport.cpp

@brief	H:4 byte-stream DeviceIntrf transport implementation.

		Converts UART or CDC byte streams to HCI packets, handles framing and
		resynchronization, and exposes the packet-oriented DeviceIntrf adapter.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_intrf_transport.h"

#include <string.h>

#define HCI_INTRF_MAX_RX_PASSES 64U
#define HCI_INTRF_MAX_TX_PASSES 8U
#define HCI_INTRF_FLUSH_PASSES 64U

static bool HciIntrfPacketTypeValid(HciH4PacketType_t Type)
{
    return Type >= HCI_H4_PACKET_COMMAND && Type <= HCI_H4_PACKET_ISO;
}

bool HciIntrfTransportSuspect(const HciIntrfTransport_t *pTransport)
{
    return pTransport != nullptr &&
           pTransport->Parser.InvalidTypeCount != pTransport->RejectedMark;
}

static void HciIntrfTransportRecordPacket(HciIntrfTransport_t *pTransport,
                                          HciH4PacketType_t Type,
                                          const uint8_t *pPacket,
                                          size_t PacketLen,
                                          bool Suspect)
{
    if (pTransport->PktMarkLen < HCI_INTRF_PKT_MARKS)
    {
        const uint8_t slot = pTransport->PktMarkLen;
        pTransport->PktMark[slot].Type = (uint8_t)Type;
        pTransport->PktMark[slot].Head[0] = PacketLen > 0U ? pPacket[0] : 0U;
        pTransport->PktMark[slot].Head[1] = PacketLen > 1U ? pPacket[1] : 0U;
        pTransport->PktMark[slot].Suspect = Suspect;
        pTransport->PktMarkLen++;
    }

    if (Suspect)
    {
        pTransport->SuspectPacketCount++;
    }

    pTransport->RxPacketCount++;
}

static bool HciIntrfTransportCount(void *pContext,
                                   HciH4PacketType_t Type,
                                   const uint8_t *pPacket,
                                   size_t PacketLen)
{
    HciIntrfTransport_t *pTransport =
        static_cast<HciIntrfTransport_t *>(pContext);
    const bool suspect = HciIntrfTransportSuspect(pTransport);

    if (suspect && Type == HCI_H4_PACKET_COMMAND &&
        pTransport->SuspectFilter != nullptr &&
        !pTransport->SuspectFilter(pTransport->pFilterContext,
                                   Type,
                                   pPacket,
                                   PacketLen))
    {
        pTransport->DroppedPacketCount++;
        return true;
    }

    if (!pTransport->Handler(pTransport->pHandlerContext,
                             Type,
                             pPacket,
                             PacketLen))
    {
        return false;
    }

    HciIntrfTransportRecordPacket(pTransport,
                                  Type,
                                  pPacket,
                                  PacketLen,
                                  suspect);
    return true;
}

static void HciIntrfPacketDisable(DevIntrf_t * const pDevIntrf);
static void HciIntrfPacketEnable(DevIntrf_t * const pDevIntrf);
static uint32_t HciIntrfPacketGetRate(DevIntrf_t * const pDevIntrf);
static uint32_t HciIntrfPacketSetRate(DevIntrf_t * const pDevIntrf,
                                     uint32_t Rate);
static bool HciIntrfPacketStartRx(DevIntrf_t * const pDevIntrf,
                                  uint32_t DevAddr);
static int HciIntrfPacketRxData(DevIntrf_t * const pDevIntrf,
                                uint8_t *pBuffer,
                                int BufferLen);
static void HciIntrfPacketStopRx(DevIntrf_t * const pDevIntrf);
static bool HciIntrfPacketStartTx(DevIntrf_t * const pDevIntrf,
                                  uint32_t DevAddr);
static int HciIntrfPacketTxData(DevIntrf_t * const pDevIntrf,
                                const uint8_t *pData,
                                int DataLen);
static int HciIntrfPacketTxSrData(DevIntrf_t * const pDevIntrf,
                                  const uint8_t *pData,
                                  int DataLen);
static void HciIntrfPacketStopTx(DevIntrf_t * const pDevIntrf);
static void HciIntrfPacketReset(DevIntrf_t * const pDevIntrf);
static void HciIntrfPacketPowerOff(DevIntrf_t * const pDevIntrf);
static void *HciIntrfPacketGetHandle(DevIntrf_t * const pDevIntrf);

static void HciIntrfPacketInitDev(HciIntrfTransport_t *pTransport)
{
    DevIntrf_t *pDev = &pTransport->DevIntrf;
    pDev->pDevData = pTransport;
    pDev->IntPrio = pTransport->pIntrf->IntPrio;
    pDev->EvtCB = pTransport->pIntrf->EvtCB;
    atomic_flag_clear(&pDev->bBusy);
    pDev->MaxRetry = 0;
    atomic_store(&pDev->EnCnt, 0);
    pDev->Type = pTransport->pIntrf->Type;
    pDev->bDma = pTransport->pIntrf->bDma;
    pDev->bIntEn = pTransport->pIntrf->bIntEn;
    atomic_store(&pDev->bTxReady, true);
    atomic_store(&pDev->bNoStop, false);
    pDev->Disable = HciIntrfPacketDisable;
    pDev->Enable = HciIntrfPacketEnable;
    pDev->GetRate = HciIntrfPacketGetRate;
    pDev->SetRate = HciIntrfPacketSetRate;
    pDev->StartRx = HciIntrfPacketStartRx;
    pDev->RxData = HciIntrfPacketRxData;
    pDev->StopRx = HciIntrfPacketStopRx;
    pDev->StartTx = HciIntrfPacketStartTx;
    pDev->TxData = HciIntrfPacketTxData;
    pDev->TxSrData = HciIntrfPacketTxSrData;
    pDev->StopTx = HciIntrfPacketStopTx;
    pDev->Reset = HciIntrfPacketReset;
    pDev->PowerOff = HciIntrfPacketPowerOff;
    pDev->GetHandle = HciIntrfPacketGetHandle;
}

bool HciIntrfTransportInit(HciIntrfTransport_t *pTransport,
                           DevIntrf_t *pIntrf,
                           uint8_t *pHciRxPacket,
                           size_t HciRxPacketCapacity,
                           HciH4PacketHandler_t PacketHandler,
                           void *pPacketContext)
{
    if (pTransport == nullptr || pIntrf == nullptr ||
        pHciRxPacket == nullptr || HciRxPacketCapacity == 0U ||
        PacketHandler == nullptr)
    {
        return false;
    }

    memset(pTransport, 0, sizeof(*pTransport));
    pTransport->pIntrf = pIntrf;
    pTransport->Handler = PacketHandler;
    pTransport->pHandlerContext = pPacketContext;

    return HciH4ParserInit(&pTransport->Parser,
                           pHciRxPacket,
                           HciRxPacketCapacity,
                           HciIntrfTransportCount,
                           pTransport);
}

bool HciIntrfTransportInitPacket(HciIntrfTransport_t *pTransport,
                                 DevIntrf_t *pIntrf,
                                 uint8_t *pHciRxPacket,
                                 size_t HciRxPacketCapacity)
{
    if (pTransport == nullptr || pIntrf == nullptr ||
        pHciRxPacket == nullptr || HciRxPacketCapacity == 0U)
    {
        return false;
    }

    memset(pTransport, 0, sizeof(*pTransport));
    pTransport->pIntrf = pIntrf;
    pTransport->PacketMode = true;
    pTransport->RxSelect = HCI_H4_PACKET_NONE;
    pTransport->TxSelect = HCI_H4_PACKET_NONE;
    HciIntrfPacketInitDev(pTransport);

    return HciH4ParserInitPassive(&pTransport->Parser,
                                  pHciRxPacket,
                                  HciRxPacketCapacity);
}

DevIntrf_t *HciIntrfTransportGetDeviceIntrf(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr || !pTransport->PacketMode)
    {
        return nullptr;
    }
    return &pTransport->DevIntrf;
}

void HciIntrfTransportSetSuspectFilter(HciIntrfTransport_t *pTransport,
                                       HciH4PacketHandler_t Filter,
                                       void *pContext)
{
    if (pTransport != nullptr)
    {
        pTransport->SuspectFilter = Filter;
        pTransport->pFilterContext = pContext;
    }
}

void HciIntrfTransportOpen(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    pTransport->Open = true;

    for (uint32_t pass = 0U; pass < HCI_INTRF_FLUSH_PASSES; pass++)
    {
        const int received = DeviceIntrfRx(pTransport->pIntrf,
                                           0U,
                                           pTransport->RxChunk,
                                           (int)sizeof(pTransport->RxChunk));
        if (received <= 0)
        {
            break;
        }

        pTransport->FlushedOctetCount += (uint32_t)received;
    }

    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    HciH4ParserReset(&pTransport->Parser);
    pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;
}

void HciIntrfTransportClose(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    pTransport->Open = false;
    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    pTransport->TxStreamLen = 0U;
    pTransport->TxStreamOffset = 0U;
    pTransport->RxSelect = HCI_H4_PACKET_NONE;
    pTransport->TxSelect = HCI_H4_PACKET_NONE;
    HciH4ParserReset(&pTransport->Parser);
}

static bool HciIntrfTransportDropPendingSuspect(HciIntrfTransport_t *pTransport)
{
    if (!pTransport->PacketMode ||
        !HciH4ParserDeliveryPending(&pTransport->Parser))
    {
        return false;
    }

    const bool suspect = HciIntrfTransportSuspect(pTransport);
    if (!suspect || pTransport->Parser.Type != HCI_H4_PACKET_COMMAND ||
        pTransport->SuspectFilter == nullptr ||
        pTransport->SuspectFilter(pTransport->pFilterContext,
                                  pTransport->Parser.Type,
                                  pTransport->Parser.pPacket,
                                  pTransport->Parser.PacketLen))
    {
        return false;
    }

    pTransport->DroppedPacketCount++;
    HciH4ParserReleasePending(&pTransport->Parser);
    return true;
}

static void HciIntrfTransportProcessRx(HciIntrfTransport_t *pTransport)
{
    for (uint32_t pass = 0U; pass < HCI_INTRF_MAX_RX_PASSES; pass++)
    {
        if (pTransport->PacketMode &&
            HciH4ParserDeliveryPending(&pTransport->Parser))
        {
            if (HciIntrfTransportDropPendingSuspect(pTransport))
            {
                continue;
            }
            return;
        }

        if (pTransport->RxChunkOffset < pTransport->RxChunkLen ||
            HciH4ParserDeliveryPending(&pTransport->Parser))
        {
            const size_t remaining =
                pTransport->RxChunkLen - pTransport->RxChunkOffset;
            const uint8_t *pData = remaining > 0U ?
                &pTransport->RxChunk[pTransport->RxChunkOffset] : nullptr;

            pTransport->RxChunkOffset +=
                HciH4ParserFeed(&pTransport->Parser, pData, remaining);

            if (pTransport->PacketMode &&
                HciH4ParserDeliveryPending(&pTransport->Parser))
            {
                if (HciIntrfTransportDropPendingSuspect(pTransport))
                {
                    continue;
                }
                return;
            }

            if (pTransport->RxChunkOffset < pTransport->RxChunkLen ||
                HciH4ParserDeliveryPending(&pTransport->Parser))
            {
                return;
            }

            pTransport->RxChunkLen = 0U;
            pTransport->RxChunkOffset = 0U;
        }

        const int received = DeviceIntrfRx(pTransport->pIntrf,
                                           0U,
                                           pTransport->RxChunk,
                                           (int)sizeof(pTransport->RxChunk));
        if (received < 0)
        {
            pTransport->RxErrorCount++;
            return;
        }

        if (received == 0)
        {
            if (!HciH4ParserIsMidPacket(&pTransport->Parser) &&
                HciIntrfTransportSuspect(pTransport))
            {
                pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;
                pTransport->SuspectClearCount++;
            }
            return;
        }

        if (pTransport->FirstRxLen < sizeof(pTransport->FirstRx))
        {
            size_t room = sizeof(pTransport->FirstRx) - pTransport->FirstRxLen;
            size_t keep = (size_t)received;
            if (keep > room)
            {
                keep = room;
            }
            memcpy(&pTransport->FirstRx[pTransport->FirstRxLen],
                   pTransport->RxChunk,
                   keep);
            pTransport->FirstRxLen += (uint8_t)keep;
        }

        pTransport->RxOctetCount += (uint32_t)received;
        pTransport->RxChunkLen = (size_t)received;
        pTransport->RxChunkOffset = 0U;
    }
}

void HciIntrfTransportIdle(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr)
    {
        return;
    }

    const bool suspect = HciIntrfTransportSuspect(pTransport);

    if (!HciH4ParserIsMidPacket(&pTransport->Parser))
    {
        pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;
        return;
    }

    if (!suspect)
    {
        return;
    }

    pTransport->RejectedMark = pTransport->Parser.InvalidTypeCount;
    HciH4ParserReset(&pTransport->Parser);
    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    pTransport->ResyncCount++;
}

static void HciIntrfTransportProcessTx(HciIntrfTransport_t *pTransport)
{
    for (uint32_t pass = 0U;
         pass < HCI_INTRF_MAX_TX_PASSES && pTransport->TxStreamLen != 0U;
         pass++)
    {
        const size_t remaining =
            pTransport->TxStreamLen - pTransport->TxStreamOffset;
        int sent = DeviceIntrfTx(pTransport->pIntrf,
                                 0U,
                                 &pTransport->TxStream[pTransport->TxStreamOffset],
                                 (int)remaining);
        if (sent < 0)
        {
            pTransport->TxErrorCount++;
            return;
        }

        if (sent == 0)
        {
            pTransport->TxBusyCount++;
            return;
        }

        pTransport->TxOctetCount += (uint32_t)sent;
        pTransport->TxStreamOffset += (size_t)sent;
        if (pTransport->TxStreamOffset >= pTransport->TxStreamLen)
        {
            pTransport->TxStreamLen = 0U;
            pTransport->TxStreamOffset = 0U;
        }
    }
}

void HciIntrfTransportProcess(HciIntrfTransport_t *pTransport)
{
    if (pTransport == nullptr || pTransport->pIntrf == nullptr ||
        !pTransport->Open)
    {
        return;
    }

    HciIntrfTransportProcessRx(pTransport);
    HciIntrfTransportProcessTx(pTransport);
}

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen)
{
    if (pTransport == nullptr || !pTransport->Open ||
        pTransport->TxStreamLen != 0U ||
        !HciIntrfPacketTypeValid(Type) ||
        PacketLen + 1U > sizeof(pTransport->TxStream) ||
        (PacketLen > 0U && pPacket == nullptr))
    {
        if (pTransport != nullptr &&
            PacketLen + 1U > sizeof(pTransport->TxStream))
        {
            pTransport->TxOversizeCount++;
        }
        return false;
    }

    pTransport->TxStream[0] = (uint8_t)Type;
    if (PacketLen > 0U)
    {
        memcpy(&pTransport->TxStream[1], pPacket, PacketLen);
    }

    pTransport->TxStreamLen = PacketLen + 1U;
    pTransport->TxStreamOffset = 0U;
    return true;
}

bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport)
{
    return pTransport != nullptr && pTransport->TxStreamLen != 0U;
}

static HciIntrfTransport_t *HciIntrfPacketFromDev(DevIntrf_t *pDevIntrf)
{
    if (pDevIntrf == nullptr || pDevIntrf->pDevData == nullptr)
    {
        return nullptr;
    }
    return static_cast<HciIntrfTransport_t *>(pDevIntrf->pDevData);
}

static void HciIntrfPacketDisable(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransportClose(HciIntrfPacketFromDev(pDevIntrf));
}

static void HciIntrfPacketEnable(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransportOpen(HciIntrfPacketFromDev(pDevIntrf));
}

static uint32_t HciIntrfPacketGetRate(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    return pTransport != nullptr ? DeviceIntrfGetRate(pTransport->pIntrf) : 0U;
}

static uint32_t HciIntrfPacketSetRate(DevIntrf_t * const pDevIntrf,
                                     uint32_t Rate)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    return pTransport != nullptr ? DeviceIntrfSetRate(pTransport->pIntrf, Rate) : 0U;
}

static bool HciIntrfPacketStartRx(DevIntrf_t * const pDevIntrf,
                                  uint32_t DevAddr)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    const HciH4PacketType_t Type = (HciH4PacketType_t)DevAddr;
    if (pTransport == nullptr || !pTransport->PacketMode || !pTransport->Open ||
        !HciIntrfPacketTypeValid(Type))
    {
        return false;
    }

    HciIntrfTransportProcessRx(pTransport);
    if (!HciH4ParserDeliveryPending(&pTransport->Parser) ||
        pTransport->Parser.Type != Type)
    {
        return false;
    }

    pTransport->RxSelect = Type;
    return true;
}

static int HciIntrfPacketRxData(DevIntrf_t * const pDevIntrf,
                                uint8_t *pBuffer,
                                int BufferLen)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    if (pTransport == nullptr || pBuffer == nullptr || BufferLen <= 0 ||
        !HciH4ParserDeliveryPending(&pTransport->Parser) ||
        pTransport->Parser.Type != pTransport->RxSelect ||
        pTransport->Parser.PacketLen > (size_t)BufferLen)
    {
        return 0;
    }

    const HciH4PacketType_t Type = pTransport->Parser.Type;
    const size_t PacketLen = pTransport->Parser.PacketLen;
    const bool suspect = HciIntrfTransportSuspect(pTransport);

    memmove(pBuffer, pTransport->Parser.pPacket, PacketLen);
    HciIntrfTransportRecordPacket(pTransport,
                                  Type,
                                  pTransport->Parser.pPacket,
                                  PacketLen,
                                  suspect);
    HciH4ParserReleasePending(&pTransport->Parser);
    return (int)PacketLen;
}

static void HciIntrfPacketStopRx(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    if (pTransport != nullptr)
    {
        pTransport->RxSelect = HCI_H4_PACKET_NONE;
    }
}

static bool HciIntrfPacketStartTx(DevIntrf_t * const pDevIntrf,
                                  uint32_t DevAddr)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    const HciH4PacketType_t Type = (HciH4PacketType_t)DevAddr;
    if (pTransport == nullptr || !pTransport->PacketMode || !pTransport->Open ||
        !HciIntrfPacketTypeValid(Type) || pTransport->TxStreamLen != 0U)
    {
        return false;
    }

    pTransport->TxSelect = Type;
    return true;
}

static int HciIntrfPacketTxData(DevIntrf_t * const pDevIntrf,
                                const uint8_t *pData,
                                int DataLen)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    if (pTransport == nullptr || pData == nullptr || DataLen <= 0 ||
        !HciIntrfPacketTypeValid(pTransport->TxSelect))
    {
        return 0;
    }

    if (!HciIntrfTransportSend(pTransport,
                               pTransport->TxSelect,
                               pData,
                               (size_t)DataLen))
    {
        return 0;
    }

    HciIntrfTransportProcessTx(pTransport);
    return DataLen;
}

static int HciIntrfPacketTxSrData(DevIntrf_t * const pDevIntrf,
                                  const uint8_t *pData,
                                  int DataLen)
{
    return HciIntrfPacketTxData(pDevIntrf, pData, DataLen);
}

static void HciIntrfPacketStopTx(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    if (pTransport != nullptr)
    {
        pTransport->TxSelect = HCI_H4_PACKET_NONE;
    }
}

static void HciIntrfPacketReset(DevIntrf_t * const pDevIntrf)
{
    HciIntrfTransport_t *pTransport = HciIntrfPacketFromDev(pDevIntrf);
    if (pTransport == nullptr)
    {
        return;
    }

    pTransport->RxChunkLen = 0U;
    pTransport->RxChunkOffset = 0U;
    pTransport->TxStreamLen = 0U;
    pTransport->TxStreamOffset = 0U;
    pTransport->RxSelect = HCI_H4_PACKET_NONE;
    pTransport->TxSelect = HCI_H4_PACKET_NONE;
    HciH4ParserReset(&pTransport->Parser);
}

static void HciIntrfPacketPowerOff(DevIntrf_t * const pDevIntrf)
{
    HciIntrfPacketDisable(pDevIntrf);
}

static void *HciIntrfPacketGetHandle(DevIntrf_t * const pDevIntrf)
{
    return HciIntrfPacketFromDev(pDevIntrf);
}

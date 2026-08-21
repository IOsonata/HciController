/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_INTRF_TRANSPORT_H
#define HCI_INTRF_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_intrf.h"
#include "hci_h4.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HCI_INTRF_IO_CHUNK_SIZE 64U
#define HCI_INTRF_TX_STREAM_SIZE 1025U
#define HCI_INTRF_PKT_MARKS 24U
#define HCI_INTRF_FIRST_RX_SIZE 64U

typedef struct {
    HciH4Parser_t Parser;

    /* Underlying byte-stream DeviceIntrf (UART, CDC, etc.). */
    DevIntrf_t *pIntrf;

    /*
     * Optional packet-facing DeviceIntrf adapter. In packet mode DevAddr is
     * HciH4PacketType_t and pData never contains the H:4 indicator.
     */
    DevIntrf_t DevIntrf;
    bool PacketMode;
    HciH4PacketType_t RxSelect;
    HciH4PacketType_t TxSelect;

    /* Legacy callback mode retained for existing users/tests. */
    HciH4PacketHandler_t Handler;
    void *pHandlerContext;

    HciH4PacketHandler_t SuspectFilter;
    void *pFilterContext;

    uint8_t RxChunk[HCI_INTRF_IO_CHUNK_SIZE];
    size_t RxChunkLen;
    size_t RxChunkOffset;

    uint8_t TxStream[HCI_INTRF_TX_STREAM_SIZE];
    size_t TxStreamLen;
    size_t TxStreamOffset;

    bool Open;

    uint32_t RxErrorCount;
    uint32_t TxErrorCount;
    uint32_t TxBusyCount;
    uint32_t TxOversizeCount;
    uint32_t RxOctetCount;
    uint32_t TxOctetCount;
    uint32_t RxPacketCount;
    uint32_t ResyncCount;
    uint32_t RejectedMark;
    uint32_t SuspectPacketCount;
    uint32_t DroppedPacketCount;
    uint32_t SuspectClearCount;
    uint32_t FlushedOctetCount;

    struct {
        uint8_t Type;
        uint8_t Head[2];
        bool Suspect;
    } PktMark[HCI_INTRF_PKT_MARKS];
    uint8_t PktMarkLen;

    uint8_t FirstRx[HCI_INTRF_FIRST_RX_SIZE];
    uint8_t FirstRxLen;
} HciIntrfTransport_t;

/* Existing callback-oriented H:4 transport API. */
bool HciIntrfTransportInit(HciIntrfTransport_t *pTransport,
                           DevIntrf_t *pIntrf,
                           uint8_t *pHciRxPacket,
                           size_t HciRxPacketCapacity,
                           HciH4PacketHandler_t PacketHandler,
                           void *pPacketContext);

/*
 * H:4 byte stream -> packet DeviceIntrf adapter.
 *
 * The returned DeviceIntrf preserves the IOsonata interface model at packet
 * level: DevAddr selects HciH4PacketType_t, one successful Rx/Tx is one HCI
 * packet, and the H:4 indicator exists only on the wrapped byte stream.
 */
bool HciIntrfTransportInitPacket(HciIntrfTransport_t *pTransport,
                                 DevIntrf_t *pIntrf,
                                 uint8_t *pHciRxPacket,
                                 size_t HciRxPacketCapacity);
DevIntrf_t *HciIntrfTransportGetDeviceIntrf(HciIntrfTransport_t *pTransport);

void HciIntrfTransportOpen(HciIntrfTransport_t *pTransport);
void HciIntrfTransportClose(HciIntrfTransport_t *pTransport);
void HciIntrfTransportProcess(HciIntrfTransport_t *pTransport);
void HciIntrfTransportIdle(HciIntrfTransport_t *pTransport);
bool HciIntrfTransportSuspect(const HciIntrfTransport_t *pTransport);
void HciIntrfTransportSetSuspectFilter(HciIntrfTransport_t *pTransport,
                                       HciH4PacketHandler_t Filter,
                                       void *pContext);

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen);
bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport);

#ifdef __cplusplus
}
#endif

#endif

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

typedef struct {
    HciH4Parser_t Parser;
    DevIntrf_t *pIntrf;

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
} HciIntrfTransport_t;

bool HciIntrfTransportInit(HciIntrfTransport_t *pTransport,
                           DevIntrf_t *pIntrf,
                           uint8_t *pHciRxPacket,
                           size_t HciRxPacketCapacity,
                           HciH4PacketHandler_t PacketHandler,
                           void *pPacketContext);

void HciIntrfTransportOpen(HciIntrfTransport_t *pTransport);
void HciIntrfTransportClose(HciIntrfTransport_t *pTransport);
void HciIntrfTransportProcess(HciIntrfTransport_t *pTransport);

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen);

bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport);

#ifdef __cplusplus
}
#endif

#endif /* HCI_INTRF_TRANSPORT_H */

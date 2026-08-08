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

    /*
     * Octets that crossed the port, counted here because this is the last
     * place they are still octets rather than packets.
     *
     * They answer a question no packet count can: whether anything arrived at
     * all. A host that is not talking and a host whose framing is wrong both
     * produce zero packets, and the two want different things looked at.
     */
    uint32_t RxOctetCount;
    uint32_t TxOctetCount;

/*
 * The first octets that ever arrived, kept so they can be looked at.
 *
 * A count says something is on the wire. It cannot say what, and the answers
 * are not close together: a host stack talking H:4, a log console on the wrong
 * wire, and an unconnected pin picking up noise all read as a busy link. The
 * octets themselves separate them at a glance, and only the first ones are
 * needed, so this is captured once and never again.
 */
#define HCI_INTRF_FIRST_RX_SIZE 24U
    uint8_t FirstRx[HCI_INTRF_FIRST_RX_SIZE];
    uint8_t FirstRxLen;
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

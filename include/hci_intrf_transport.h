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

    /*
     * The parser calls this layer and this layer calls on, so an accepted
     * packet can be counted where it is known to have been accepted.
     */
    HciH4PacketHandler_t Handler;
    void *pHandlerContext;

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

    /* Packets that reached the handler, and packets abandoned mid way. */
    uint32_t RxPacketCount;
    uint32_t ResyncCount;

/*
 * The first octets that ever arrived, kept so they can be looked at.
 *
 * A count says something is on the wire. It cannot say what, and the answers
 * are not close together: a host stack talking H:4, a log console on the wrong
 * wire, and an unconnected pin picking up noise all read as a busy link. The
 * octets themselves separate them at a glance.
 *
 * Filled across as many reads as it takes rather than from one of them. A
 * first read that returned three octets gave three octets to look at, which
 * was enough to see that the wire held text and not enough to see whose text
 * it was. Long enough now to hold a line of it.
 */
#define HCI_INTRF_FIRST_RX_SIZE 64U
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

/*
 * The link has been quiet long enough that nothing can still be arriving, so
 * anything half built is not going to be finished. Throw it away and take the
 * next octet as the start of a packet.
 *
 * This is the only thing that can recover a stream that once held something
 * other than H:4. There is no delimiter in H:4 and no length worth checking,
 * so an octet of foreign data that happens to look like an indicator makes
 * this side read a header and a payload behind it, and a payload length taken
 * from text is usually long enough to swallow whatever real packet comes next.
 * Nothing in the octets ever says so.
 *
 * The gap is the one thing that does. A host sends a packet in one go, so a
 * silence of many octet times means the previous packet ended, whatever this
 * side believes about it. The caller decides how long is long enough, since
 * only it knows the rate.
 *
 * Safe when nothing is half built, which is the ordinary case: a parser
 * sitting at a packet boundary has nothing to throw away, and this counts
 * nothing.
 */
void HciIntrfTransportIdle(HciIntrfTransport_t *pTransport);

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen);

bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport);

#ifdef __cplusplus
}
#endif

#endif /* HCI_INTRF_TRANSPORT_H */

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
     * Whether the stream has shown itself not to be H:4 since it last went
     * quiet. Recorded against every packet built out of it, and not acted on.
     *
     * A real H:4 stream never holds an octet outside the indicator range at a
     * packet boundary. One that does is either not H:4 or is being read from
     * the wrong place, and in both cases every packet that follows it in the
     * same burst is likely an accident of where the reading started. Refusing
     * those was tried and cost eight real HCI Resets, so it is a label now and
     * not a decision.
     *
     * Held as the rejected count at the last quiet moment rather than as a
     * flag, because the flag has to be right at the instant a packet is
     * handed over and the parser hands it over from inside the same call that
     * rejected the octets. A mark compared on the spot cannot be set too late;
     * a flag set after the call already was.
     */
    uint32_t RejectedMark;
    uint32_t SuspectPacketCount;

    /*
     * Octets thrown away at open, because they arrived while the driver was
     * configured and nothing behind it could answer yet. A large number here
     * is ordinary on a board whose peer talks during its own start up.
     */
    uint32_t FlushedOctetCount;

/*
 * The first packets this side built, whether they were handed on or thrown
 * away, as the indicator and the two octets behind it. For a command those two
 * are the opcode, so 01 03 0C is an HCI Reset arriving and is the one thing
 * worth being certain about on a link that is not working.
 *
 * Each is flagged with whether the stream it came out of had already shown
 * itself not to be H:4. That flag used to decide whether the packet was thrown
 * away and now only describes it, because throwing them away threw away real
 * HCI Resets along with the accidents.
 *
 * Three octets rather than the packet, because what is wanted here is which
 * packet it was, not what was in it.
 */
#define HCI_INTRF_PKT_MARKS 8U
    struct {
        uint8_t Type;
        uint8_t Head[2];
        bool Suspect;
    } PktMark[HCI_INTRF_PKT_MARKS];
    uint8_t PktMarkLen;

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

/*
 * Whether the stream has shown itself not to be H:4 since it last went quiet.
 * Packets built out of such a stream are labelled and still delivered.
 */
bool HciIntrfTransportSuspect(const HciIntrfTransport_t *pTransport);

bool HciIntrfTransportSend(HciIntrfTransport_t *pTransport,
                           HciH4PacketType_t Type,
                           const uint8_t *pPacket,
                           size_t PacketLen);

bool HciIntrfTransportTxBusy(const HciIntrfTransport_t *pTransport);

#ifdef __cplusplus
}
#endif

#endif /* HCI_INTRF_TRANSPORT_H */

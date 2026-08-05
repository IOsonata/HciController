/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_H4_H
#define HCI_H4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HCI_H4_PACKET_NONE    = 0x00,
    HCI_H4_PACKET_COMMAND = 0x01,
    HCI_H4_PACKET_ACL     = 0x02,
    HCI_H4_PACKET_SCO     = 0x03,
    HCI_H4_PACKET_EVENT   = 0x04,
    HCI_H4_PACKET_ISO     = 0x05,
} HciH4PacketType_t;

/*
 * Return true when the packet has been accepted. Returning false keeps the
 * completed packet in the parser and applies backpressure to the input stream.
 */
typedef bool (*HciH4PacketHandler_t)(void *pContext,
                                     HciH4PacketType_t Type,
                                     const uint8_t *pPacket,
                                     size_t PacketLen);

typedef enum {
    HCI_H4_PARSE_TYPE = 0,
    HCI_H4_PARSE_HEADER,
    HCI_H4_PARSE_PAYLOAD,
    HCI_H4_PARSE_DELIVER,
    HCI_H4_PARSE_DROP,
} HciH4ParseState_t;

typedef struct {
    uint8_t *pPacket;
    size_t PacketCapacity;
    size_t PacketLen;
    size_t HeaderLen;
    size_t PayloadLen;
    size_t DropRemaining;
    HciH4PacketHandler_t Handler;
    void *pContext;
    HciH4PacketType_t Type;
    HciH4ParseState_t State;
    uint32_t InvalidTypeCount;
    uint32_t OversizePacketCount;
    uint32_t DeliveryRetryCount;
} HciH4Parser_t;

bool HciH4ParserInit(HciH4Parser_t *pParser,
                     uint8_t *pPacket,
                     size_t PacketCapacity,
                     HciH4PacketHandler_t Handler,
                     void *pContext);

void HciH4ParserReset(HciH4Parser_t *pParser);

/*
 * Feed bytes into the parser and return the number consumed. DataLen may be
 * zero with pData equal to NULL to retry a blocked packet delivery.
 */
size_t HciH4ParserFeed(HciH4Parser_t *pParser,
                       const uint8_t *pData,
                       size_t DataLen);

bool HciH4ParserDeliveryPending(const HciH4Parser_t *pParser);

#ifdef __cplusplus
}
#endif

#endif

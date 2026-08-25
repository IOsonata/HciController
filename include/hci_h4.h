/**-------------------------------------------------------------------------
@file	hci_h4.h

@brief	Bluetooth H:4 packet parser definitions.

		Defines H:4 packet types, parser state, callbacks, and active/passive
		parser APIs used by byte-stream Host transports.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

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

/*
 * Packet-interface mode. The parser stops in DELIVER with the completed packet
 * in pPacket instead of invoking a callback. The owner reads Type/PacketLen and
 * calls HciH4ParserReleasePending() after the packet has been copied out.
 */
bool HciH4ParserInitPassive(HciH4Parser_t *pParser,
                            uint8_t *pPacket,
                            size_t PacketCapacity);

void HciH4ParserReset(HciH4Parser_t *pParser);
void HciH4ParserReleasePending(HciH4Parser_t *pParser);

/*
 * Feed bytes into the parser and return the number consumed. DataLen may be
 * zero with pData equal to NULL to retry a blocked packet delivery.
 */
size_t HciH4ParserFeed(HciH4Parser_t *pParser,
                       const uint8_t *pData,
                       size_t DataLen);

bool HciH4ParserDeliveryPending(const HciH4Parser_t *pParser);

bool HciH4ParserIsMidPacket(const HciH4Parser_t *pParser);

#ifdef __cplusplus
}
#endif

#endif

/**-------------------------------------------------------------------------
@file	hci_h4.cpp

@brief	Bluetooth H:4 packet parser implementation.

		Parses command, ACL, SCO, event, and ISO packets from byte streams,
		handles oversize packet drops, and supports active or passive delivery.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_h4.h"

#include <string.h>

static size_t HciH4HeaderLen(HciH4PacketType_t Type)
{
    switch (Type)
    {
        case HCI_H4_PACKET_COMMAND:
        case HCI_H4_PACKET_SCO:
            return 3;

        case HCI_H4_PACKET_ACL:
        case HCI_H4_PACKET_ISO:
            return 4;

        case HCI_H4_PACKET_EVENT:
            return 2;

        default:
            return 0;
    }
}

static size_t HciH4PayloadLen(const HciH4Parser_t *pParser)
{
    const uint8_t *pHeader = pParser->pPacket;

    switch (pParser->Type)
    {
        case HCI_H4_PACKET_COMMAND:
        case HCI_H4_PACKET_SCO:
            return pHeader[2];

        case HCI_H4_PACKET_ACL:
            return (size_t)pHeader[2] | ((size_t)pHeader[3] << 8);

        case HCI_H4_PACKET_EVENT:
            return pHeader[1];

        case HCI_H4_PACKET_ISO:
            return ((size_t)pHeader[2] | ((size_t)pHeader[3] << 8)) & 0x3FFFU;

        default:
            return 0;
    }
}

static void HciH4BeginPacket(HciH4Parser_t *pParser, HciH4PacketType_t Type)
{
    pParser->Type = Type;
    pParser->HeaderLen = HciH4HeaderLen(Type);
    pParser->PayloadLen = 0;
    pParser->PacketLen = 0;
    pParser->DropRemaining = 0;
    pParser->State = HCI_H4_PARSE_HEADER;
}

static void HciH4ReleasePacket(HciH4Parser_t *pParser)
{
    pParser->Type = HCI_H4_PACKET_NONE;
    pParser->PacketLen = 0;
    pParser->HeaderLen = 0;
    pParser->PayloadLen = 0;
    pParser->DropRemaining = 0;
    pParser->State = HCI_H4_PARSE_TYPE;
}

static bool HciH4DeliverPacket(HciH4Parser_t *pParser)
{
    /*
     * Passive mode is used by a packet DeviceIntrf adapter. Leaving the parser
     * in DELIVER is not a delivery retry; the packet is simply waiting for the
     * DeviceIntrf consumer to copy it and explicitly release it.
     */
    if (pParser->Handler == NULL)
    {
        return false;
    }

    if (!pParser->Handler(pParser->pContext,
                          pParser->Type,
                          pParser->pPacket,
                          pParser->PacketLen))
    {
        pParser->DeliveryRetryCount++;
        return false;
    }

    HciH4ReleasePacket(pParser);
    return true;
}

static bool HciH4ParserInitCommon(HciH4Parser_t *pParser,
                                  uint8_t *pPacket,
                                  size_t PacketCapacity,
                                  HciH4PacketHandler_t Handler,
                                  void *pContext,
                                  bool HandlerRequired)
{
    if (pParser == NULL || pPacket == NULL || PacketCapacity < 4U ||
        (HandlerRequired && Handler == NULL))
    {
        return false;
    }

    memset(pParser, 0, sizeof(*pParser));
    pParser->pPacket = pPacket;
    pParser->PacketCapacity = PacketCapacity;
    pParser->Handler = Handler;
    pParser->pContext = pContext;
    pParser->State = HCI_H4_PARSE_TYPE;
    return true;
}

bool HciH4ParserInit(HciH4Parser_t *pParser,
                     uint8_t *pPacket,
                     size_t PacketCapacity,
                     HciH4PacketHandler_t Handler,
                     void *pContext)
{
    return HciH4ParserInitCommon(pParser,
                                 pPacket,
                                 PacketCapacity,
                                 Handler,
                                 pContext,
                                 true);
}

bool HciH4ParserInitPassive(HciH4Parser_t *pParser,
                            uint8_t *pPacket,
                            size_t PacketCapacity)
{
    return HciH4ParserInitCommon(pParser,
                                 pPacket,
                                 PacketCapacity,
                                 NULL,
                                 NULL,
                                 false);
}

void HciH4ParserReset(HciH4Parser_t *pParser)
{
    if (pParser == NULL)
    {
        return;
    }

    HciH4ReleasePacket(pParser);
}

void HciH4ParserReleasePending(HciH4Parser_t *pParser)
{
    if (pParser != NULL && pParser->State == HCI_H4_PARSE_DELIVER)
    {
        HciH4ReleasePacket(pParser);
    }
}

size_t HciH4ParserFeed(HciH4Parser_t *pParser,
                       const uint8_t *pData,
                       size_t DataLen)
{
    if (pParser == NULL || (pData == NULL && DataLen > 0U))
    {
        return 0;
    }

    size_t offset = 0;

    for (;;)
    {
        if (pParser->State == HCI_H4_PARSE_DELIVER)
        {
            if (!HciH4DeliverPacket(pParser))
            {
                return offset;
            }
            continue;
        }

        if (offset >= DataLen)
        {
            break;
        }

        switch (pParser->State)
        {
            case HCI_H4_PARSE_TYPE:
            {
                HciH4PacketType_t type = (HciH4PacketType_t)pData[offset++];
                if (HciH4HeaderLen(type) == 0U)
                {
                    pParser->InvalidTypeCount++;
                    break;
                }

                HciH4BeginPacket(pParser, type);
                break;
            }

            case HCI_H4_PARSE_HEADER:
            {
                size_t remaining = pParser->HeaderLen - pParser->PacketLen;
                size_t available = DataLen - offset;
                size_t copyLen = remaining < available ? remaining : available;

                memcpy(&pParser->pPacket[pParser->PacketLen], &pData[offset], copyLen);
                pParser->PacketLen += copyLen;
                offset += copyLen;

                if (pParser->PacketLen == pParser->HeaderLen)
                {
                    pParser->PayloadLen = HciH4PayloadLen(pParser);
                    size_t totalLen = pParser->HeaderLen + pParser->PayloadLen;

                    if (totalLen > pParser->PacketCapacity)
                    {
                        pParser->OversizePacketCount++;
                        pParser->DropRemaining = pParser->PayloadLen;
                        pParser->PacketLen = 0;
                        pParser->State = pParser->DropRemaining == 0U ?
                                         HCI_H4_PARSE_TYPE : HCI_H4_PARSE_DROP;
                    }
                    else if (pParser->PayloadLen == 0U)
                    {
                        pParser->State = HCI_H4_PARSE_DELIVER;
                    }
                    else
                    {
                        pParser->State = HCI_H4_PARSE_PAYLOAD;
                    }
                }
                break;
            }

            case HCI_H4_PARSE_PAYLOAD:
            {
                size_t totalLen = pParser->HeaderLen + pParser->PayloadLen;
                size_t remaining = totalLen - pParser->PacketLen;
                size_t available = DataLen - offset;
                size_t copyLen = remaining < available ? remaining : available;

                memcpy(&pParser->pPacket[pParser->PacketLen], &pData[offset], copyLen);
                pParser->PacketLen += copyLen;
                offset += copyLen;

                if (pParser->PacketLen == totalLen)
                {
                    pParser->State = HCI_H4_PARSE_DELIVER;
                }
                break;
            }

            case HCI_H4_PARSE_DROP:
            {
                size_t available = DataLen - offset;
                size_t dropLen = pParser->DropRemaining < available ?
                                 pParser->DropRemaining : available;
                pParser->DropRemaining -= dropLen;
                offset += dropLen;

                if (pParser->DropRemaining == 0U)
                {
                    HciH4ReleasePacket(pParser);
                }
                break;
            }

            default:
                HciH4ParserReset(pParser);
                break;
        }
    }

    return offset;
}

bool HciH4ParserDeliveryPending(const HciH4Parser_t *pParser)
{
    return pParser != NULL && pParser->State == HCI_H4_PARSE_DELIVER;
}

bool HciH4ParserIsMidPacket(const HciH4Parser_t *pParser)
{
    if (pParser == NULL)
    {
        return false;
    }

    return pParser->State == HCI_H4_PARSE_HEADER ||
           pParser->State == HCI_H4_PARSE_PAYLOAD ||
           pParser->State == HCI_H4_PARSE_DROP;
}

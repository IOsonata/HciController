/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_h4.h"

#include <assert.h>
#include <string.h>

struct Capture
{
    unsigned Count;
    HciH4PacketType_t Type[8];
    size_t Len[8];
    uint8_t Data[8][32];
};

static bool CapturePacket(void *pContext,
                          HciH4PacketType_t Type,
                          const uint8_t *pPacket,
                          size_t PacketLen)
{
    Capture *capture = static_cast<Capture *>(pContext);
    assert(capture->Count < 8);
    assert(PacketLen <= sizeof(capture->Data[0]));

    unsigned index = capture->Count++;
    capture->Type[index] = Type;
    capture->Len[index] = PacketLen;
    memcpy(capture->Data[index], pPacket, PacketLen);
    return true;
}

struct BlockingCapture
{
    Capture CaptureData;
    bool Accept;
    unsigned Attempts;
};

static bool CaptureWithBackpressure(void *pContext,
                                    HciH4PacketType_t Type,
                                    const uint8_t *pPacket,
                                    size_t PacketLen)
{
    BlockingCapture *blocking = static_cast<BlockingCapture *>(pContext);
    blocking->Attempts++;
    if (!blocking->Accept)
    {
        return false;
    }

    return CapturePacket(&blocking->CaptureData, Type, pPacket, PacketLen);
}

int main()
{
    uint8_t packet[32];
    Capture capture = {};
    HciH4Parser_t parser;

    assert(HciH4ParserInit(&parser, packet, sizeof(packet), CapturePacket, &capture));

    const uint8_t reset1[] = { HCI_H4_PACKET_COMMAND, 0x03 };
    const uint8_t reset2[] = { 0x0C, 0x00 };
    assert(HciH4ParserFeed(&parser, reset1, sizeof(reset1)) == sizeof(reset1));
    assert(capture.Count == 0);
    assert(HciH4ParserFeed(&parser, reset2, sizeof(reset2)) == sizeof(reset2));
    assert(capture.Count == 1);
    assert(capture.Type[0] == HCI_H4_PACKET_COMMAND);
    assert(capture.Len[0] == 3);
    assert(capture.Data[0][0] == 0x03 && capture.Data[0][1] == 0x0C && capture.Data[0][2] == 0x00);

    const uint8_t combined[] = {
        0x99,
        HCI_H4_PACKET_ACL, 0x01, 0x20, 0x03, 0x00, 0xAA, 0xBB, 0xCC,
        HCI_H4_PACKET_EVENT, 0x0E, 0x01, 0x00,
    };
    assert(HciH4ParserFeed(&parser, combined, sizeof(combined)) == sizeof(combined));
    assert(parser.InvalidTypeCount == 1);
    assert(capture.Count == 3);
    assert(capture.Type[1] == HCI_H4_PACKET_ACL && capture.Len[1] == 7);
    assert(capture.Type[2] == HCI_H4_PACKET_EVENT && capture.Len[2] == 3);

    uint8_t smallPacket[8];
    Capture smallCapture = {};
    HciH4Parser_t smallParser;
    assert(HciH4ParserInit(&smallParser, smallPacket, sizeof(smallPacket), CapturePacket, &smallCapture));

    const uint8_t oversizedThenReset[] = {
        HCI_H4_PACKET_ACL, 0x01, 0x20, 0x06, 0x00,
        1, 2, 3, 4, 5, 6,
        HCI_H4_PACKET_COMMAND, 0x03, 0x0C, 0x00,
    };
    assert(HciH4ParserFeed(&smallParser, oversizedThenReset, sizeof(oversizedThenReset)) == sizeof(oversizedThenReset));
    assert(smallParser.OversizePacketCount == 1);
    assert(smallCapture.Count == 1);
    assert(smallCapture.Type[0] == HCI_H4_PACKET_COMMAND);

    const uint8_t iso[] = {
        HCI_H4_PACKET_ISO, 0x01, 0x00, 0x03, 0xC0, 0x11, 0x22, 0x33,
    };
    assert(HciH4ParserFeed(&parser, iso, sizeof(iso)) == sizeof(iso));
    assert(capture.Count == 4);
    assert(capture.Type[3] == HCI_H4_PACKET_ISO && capture.Len[3] == 7);

    uint8_t blockedPacket[32];
    BlockingCapture blocked = {};
    HciH4Parser_t blockedParser;
    assert(HciH4ParserInit(&blockedParser, blockedPacket, sizeof(blockedPacket),
                           CaptureWithBackpressure, &blocked));

    const uint8_t twoCommands[] = {
        HCI_H4_PACKET_COMMAND, 0x03, 0x0C, 0x00,
        HCI_H4_PACKET_COMMAND, 0x01, 0x10, 0x00,
    };
    assert(HciH4ParserFeed(&blockedParser, twoCommands, sizeof(twoCommands)) == 4);
    assert(HciH4ParserDeliveryPending(&blockedParser));
    assert(blocked.Attempts == 1);
    assert(blocked.CaptureData.Count == 0);

    blocked.Accept = true;
    assert(HciH4ParserFeed(&blockedParser, &twoCommands[4], 4) == 4);
    assert(blocked.Attempts == 3);
    assert(blocked.CaptureData.Count == 2);
    assert(!HciH4ParserDeliveryPending(&blockedParser));

    return 0;
}

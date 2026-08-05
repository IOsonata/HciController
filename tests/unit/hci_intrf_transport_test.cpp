/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Host test for the DevIntrf backed HCI transport.
 *
 * DeviceIntrfRx and DeviceIntrfTx are defined here rather than linked from
 * IOsonata, so the test drives short reads, short writes and a closed port
 * without any hardware. The stub device_intrf.h supplies the declarations.
 */

#include "hci_intrf_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_RX_SIZE 512
#define FAKE_TX_SIZE 2048

struct FakeIntrf
{
    DevIntrf_t Base;

    uint8_t RxData[FAKE_RX_SIZE];
    int RxLen;
    int RxOffset;
    int RxChunkLimit;   /* bytes returned per call, 0 means as many as asked */

    uint8_t TxData[FAKE_TX_SIZE];
    int TxLen;
    int TxChunkLimit;   /* bytes accepted per call, 0 means as many as offered */
    int TxFailAfter;    /* number of successful calls before returning 0 */
    int TxCallCount;

    unsigned EnableCount;
    unsigned DisableCount;
};

static FakeIntrf gIntrf;

int DeviceIntrfRx(DevIntrf_t *, uint32_t, uint8_t *pBuffer, int BufferLen)
{
    int available = gIntrf.RxLen - gIntrf.RxOffset;
    if (available <= 0 || BufferLen <= 0)
    {
        return 0;
    }

    int count = available < BufferLen ? available : BufferLen;
    if (gIntrf.RxChunkLimit > 0 && count > gIntrf.RxChunkLimit)
    {
        count = gIntrf.RxChunkLimit;
    }

    memcpy(pBuffer, &gIntrf.RxData[gIntrf.RxOffset], (size_t)count);
    gIntrf.RxOffset += count;
    return count;
}

int DeviceIntrfTx(DevIntrf_t *, uint32_t, const uint8_t *pData, int DataLen)
{
    if (DataLen <= 0)
    {
        return 0;
    }

    if (gIntrf.TxFailAfter > 0 && gIntrf.TxCallCount >= gIntrf.TxFailAfter)
    {
        gIntrf.TxCallCount++;
        return 0;
    }

    int count = DataLen;
    if (gIntrf.TxChunkLimit > 0 && count > gIntrf.TxChunkLimit)
    {
        count = gIntrf.TxChunkLimit;
    }

    assert(gIntrf.TxLen + count <= FAKE_TX_SIZE);
    memcpy(&gIntrf.TxData[gIntrf.TxLen], pData, (size_t)count);
    gIntrf.TxLen += count;
    gIntrf.TxCallCount++;
    return count;
}

void DeviceIntrfEnable(DevIntrf_t *)
{
    gIntrf.EnableCount++;
}

void DeviceIntrfDisable(DevIntrf_t *)
{
    gIntrf.DisableCount++;
}

struct Capture
{
    unsigned Count;
    bool Accept;
    HciH4PacketType_t Type;
    size_t Len;
    uint8_t Data[64];
};

static bool CapturePacket(void *pContext,
                          HciH4PacketType_t Type,
                          const uint8_t *pPacket,
                          size_t PacketLen)
{
    Capture *capture = static_cast<Capture *>(pContext);
    assert(PacketLen <= sizeof(capture->Data));

    if (!capture->Accept)
    {
        return false;
    }

    capture->Count++;
    capture->Type = Type;
    capture->Len = PacketLen;
    memcpy(capture->Data, pPacket, PacketLen);
    return true;
}

static void ResetIntrf(void)
{
    memset(&gIntrf, 0, sizeof(gIntrf));
}

static void FeedRx(const uint8_t *pData, size_t Len)
{
    assert(gIntrf.RxLen + (int)Len <= FAKE_RX_SIZE);
    memcpy(&gIntrf.RxData[gIntrf.RxLen], pData, Len);
    gIntrf.RxLen += (int)Len;
}

/* A complete command arrives in one read and reaches the handler. */
static void TestSinglePacket(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(wire, sizeof(wire));

    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(capture.Type == HCI_H4_PACKET_COMMAND);
    assert(capture.Len == 3U);
    assert(capture.Data[0] == 0x03 && capture.Data[1] == 0x0C);
    printf("[ok] single command packet reaches the handler\n");
}

/* A packet split across several short reads is reassembled. */
static void TestSplitReads(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    const uint8_t wire[] = {0x02, 0x40, 0x00, 0x04, 0x00,
                            0xAA, 0xBB, 0xCC, 0xDD};
    FeedRx(wire, sizeof(wire));
    gIntrf.RxChunkLimit = 1;

    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(capture.Type == HCI_H4_PACKET_ACL);
    assert(capture.Len == 8U);
    assert(capture.Data[7] == 0xDD);
    printf("[ok] packet split across one byte reads is reassembled\n");
}

/* A rejected packet is retried on the next process pass, not dropped. */
static void TestBackpressure(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = false;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(wire, sizeof(wire));

    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 0U);

    capture.Accept = true;
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 1U);
    assert(capture.Len == 3U);
    printf("[ok] rejected packet is retried rather than dropped\n");
}

/* Send prefixes the H:4 indicator and drains across short writes. */
static void TestSendDrains(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    const uint8_t event[] = {0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};
    assert(HciIntrfTransportSend(&transport, HCI_H4_PACKET_EVENT, event,
                                 sizeof(event)));
    assert(HciIntrfTransportTxBusy(&transport));

    /* A second send is refused while the first is still queued. */
    assert(!HciIntrfTransportSend(&transport, HCI_H4_PACKET_EVENT, event,
                                  sizeof(event)));

    gIntrf.TxChunkLimit = 2;
    HciIntrfTransportProcess(&transport);

    assert(!HciIntrfTransportTxBusy(&transport));
    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    assert(gIntrf.TxData[0] == HCI_H4_PACKET_EVENT);
    assert(memcmp(&gIntrf.TxData[1], event, sizeof(event)) == 0);
    printf("[ok] send prefixes the indicator and drains short writes\n");
}

/* An oversize packet is refused and counted rather than truncated. */
static void TestSendOversize(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];
    static uint8_t big[HCI_INTRF_TX_STREAM_SIZE];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    assert(!HciIntrfTransportSend(&transport, HCI_H4_PACKET_ACL, big,
                                  sizeof(big)));
    assert(transport.TxOversizeCount == 1U);
    assert(!HciIntrfTransportTxBusy(&transport));

    /* The largest packet that still fits is accepted. */
    assert(HciIntrfTransportSend(&transport, HCI_H4_PACKET_ACL, big,
                                 HCI_INTRF_TX_STREAM_SIZE - 1U));
    printf("[ok] oversize send is refused, boundary size is accepted\n");
}

/* A closed port neither reads nor sends. */
static void TestClosedPort(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(wire, sizeof(wire));

    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 0U);
    assert(!HciIntrfTransportSend(&transport, HCI_H4_PACKET_EVENT, wire, 4U));

    HciIntrfTransportOpen(&transport);
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 1U);

    HciIntrfTransportClose(&transport);
    assert(!HciIntrfTransportSend(&transport, HCI_H4_PACKET_EVENT, wire, 4U));
    printf("[ok] a closed port neither reads nor sends\n");
}

/* Init rejects the argument combinations that cannot work. */
static void TestInitGuards(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));

    assert(!HciIntrfTransportInit(NULL, &gIntrf.Base, packet, sizeof(packet),
                                  CapturePacket, &capture));
    assert(!HciIntrfTransportInit(&transport, NULL, packet, sizeof(packet),
                                  CapturePacket, &capture));
    assert(!HciIntrfTransportInit(&transport, &gIntrf.Base, NULL,
                                  sizeof(packet), CapturePacket, &capture));
    assert(!HciIntrfTransportInit(&transport, &gIntrf.Base, packet, 0U,
                                  CapturePacket, &capture));
    assert(!HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                  sizeof(packet), NULL, &capture));
    printf("[ok] init rejects unusable arguments\n");
}

int main(void)
{
    TestSinglePacket();
    TestSplitReads();
    TestBackpressure();
    TestSendDrains();
    TestSendOversize();
    TestClosedPort();
    TestInitGuards();

    printf("All HCI transport tests passed.\n");
    return 0;
}

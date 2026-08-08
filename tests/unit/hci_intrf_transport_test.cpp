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

/*
 * The octets are counted and the first of them kept.
 *
 * Kept once and not replaced, because the question they answer is what was on
 * the wire when this side started listening. A capture that followed the
 * stream would show the middle of whatever is there now, which is the one
 * thing already visible from the counts.
 */
static void TestFirstOctetsAreKept(void)
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

    assert(transport.RxOctetCount == 0U);
    assert(transport.FirstRxLen == 0U);

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(wire, sizeof(wire));
    HciIntrfTransportProcess(&transport);

    assert(transport.RxOctetCount == 4U);
    assert(transport.FirstRxLen == 4U);
    assert(memcmp(transport.FirstRx, wire, sizeof(wire)) == 0);

    /*
     * More arrives, and the window keeps filling across reads rather than
     * holding whatever the first one happened to return. A short first read is
     * the ordinary case on a busy wire, and taking only that gave three octets
     * to identify a stream by.
     */
    const uint8_t more[] = {0x01, 0x01, 0x10, 0x00};
    FeedRx(more, sizeof(more));
    HciIntrfTransportProcess(&transport);

    assert(transport.RxOctetCount == 8U);
    assert(transport.FirstRxLen == 8U);
    assert(memcmp(transport.FirstRx, wire, sizeof(wire)) == 0);
    assert(memcmp(&transport.FirstRx[4], more, sizeof(more)) == 0);

    /* And it stops at its size rather than past it. */
    uint8_t flood[HCI_INTRF_FIRST_RX_SIZE * 2U];
    memset(flood, 0xEE, sizeof(flood));
    FeedRx(flood, sizeof(flood));
    HciIntrfTransportProcess(&transport);

    assert(transport.FirstRxLen == HCI_INTRF_FIRST_RX_SIZE);
    assert(memcmp(transport.FirstRx, wire, sizeof(wire)) == 0);

    printf("[ok] the first %u octets are kept across reads\n",
           (unsigned)transport.FirstRxLen);
}

/*
 * A stream that is not H:4 at all is counted as octets and refused as packets.
 * That pair is what separates a busy wire from a host: an unconnected pin and
 * a console on the wrong wire both move octets and deliver nothing.
 */
static void TestNonH4StreamIsSeenAsSuch(void)
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

    const uint8_t text[] = "boot: hello from a console\n";
    FeedRx(text, sizeof(text) - 1U);
    HciIntrfTransportProcess(&transport);

    assert(transport.RxOctetCount == sizeof(text) - 1U);
    assert(capture.Count == 0U);
    assert(transport.Parser.InvalidTypeCount > 0U);
    assert(transport.FirstRx[0] == 'b');
    printf("[ok] a busy wire that is not h4 says so, %lu bad indicator(s)\n",
           (unsigned long)transport.Parser.InvalidTypeCount);
}

/*
 * A boot banner, then silence, then the first real command.
 *
 * This is the Thingy:91 exactly. The nRF9160's bootloader prints on the same
 * UART its application later uses for HCI, so this side is handed text before
 * it is handed packets. One octet of that text that happens to look like an
 * indicator makes the parser read a payload length out of more text, and a
 * length taken from text is long enough to swallow the Reset that follows.
 * Nothing in the octets says so and the host times out with no idea why.
 *
 * The gap between the banner and the first command is the only thing that
 * separates them, and on that board it is over a hundred milliseconds.
 */
static void TestBannerThenIdleThenCommand(void)
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

    /*
     * Ends with an octet that looks like an ACL indicator and a length taken
     * from the text behind it, so the parser is left waiting for a payload
     * far longer than anything that follows. Built on purpose rather than
     * hoped for, because the case only matters when it happens.
     */
    const uint8_t banner[] = {
        'A', 'l', 'l', ' ', 'p', 'i', 'n', 's', ' ', 'o', 'k', '\r', '\n',
        0x02, 0x40, 0x00, 0xFF, 0x00,
    };
    FeedRx(banner, sizeof(banner));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 0U);
    assert(HciH4ParserIsMidPacket(&transport.Parser));

    /* The real command, which without a resync is eaten as that payload. */
    const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 0U);

    /* Rewind and do it again, with the gap the board actually leaves. */
    ResetIntrf();
    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;
    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    FeedRx(banner, sizeof(banner));
    HciIntrfTransportProcess(&transport);
    assert(HciH4ParserIsMidPacket(&transport.Parser));

    HciIntrfTransportIdle(&transport);
    assert(!HciH4ParserIsMidPacket(&transport.Parser));
    assert(transport.ResyncCount == 1U);

    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(capture.Type == HCI_H4_PACKET_COMMAND);
    assert(capture.Data[0] == 0x03 && capture.Data[1] == 0x0C);
    assert(transport.RxPacketCount == 1U);

    /* And a gap at a packet boundary throws nothing away and counts nothing. */
    HciIntrfTransportIdle(&transport);
    assert(transport.ResyncCount == 1U);

    printf("[ok] a banner then a gap does not eat the command after it\n");
}

/*
 * A packet built out of text is labelled and still delivered.
 *
 * Refusing these was tried on hardware and refused eight real HCI Resets with
 * them, which is a link that never starts. Answering an accidental command
 * only costs the host a resynchronisation, and the host drains its own port
 * when it opens the transport, so answers sent before that reach nobody. Since
 * this layer cannot tell the two apart, the label travels with the packet and
 * the decision belongs to whatever can read an opcode table.
 */
static void TestTextBuiltPacketsAreLabelled(void)
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

    /* Text, then something inside it that parses as a whole command. */
    const uint8_t banner[] = {
        'B', 'o', 'o', 't', 'i', 'n', 'g', ' ',
        0x01, 0x03, 0x0C, 0x00,
        'd', 'o', 'n', 'e', '\r', '\n',
    };
    FeedRx(banner, sizeof(banner));
    HciIntrfTransportProcess(&transport);

    assert(transport.Parser.InvalidTypeCount > 0U);

    /*
     * The flag itself has already lifted, because draining the port ends the
     * burst and the burst is what suspicion is about. What was suspect at the
     * moment the packet was built is on the packet, which is where it belongs.
     */
    assert(!HciIntrfTransportSuspect(&transport));
    assert(transport.SuspectClearCount == 1U);

    /* Delivered, and marked as having come out of a stream that is not H:4. */
    assert(capture.Count == 1U);
    assert(transport.SuspectPacketCount == 1U);
    assert(transport.RxPacketCount == 1U);
    assert(transport.PktMark[0].Suspect);

    /* And the same octets after it are an ordinary command again. */
    const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 2U);
    assert(transport.RxPacketCount == 2U);
    assert(transport.SuspectPacketCount == 1U);
    assert(!transport.PktMark[1].Suspect);

    printf("[ok] a command found inside text is labelled, not lost\n");
}

/* Accepts only the one opcode, standing in for the dispatch table. */
static bool OnlyReset(void *, HciH4PacketType_t Type, const uint8_t *pPacket,
                      size_t PacketLen)
{
    return Type == HCI_H4_PACKET_COMMAND && PacketLen >= 2U &&
           pPacket[0] == 0x03U && pPacket[1] == 0x0CU;
}

/*
 * With a filter, a suspect stream loses its accidents and keeps its commands.
 *
 * Both halves have been wrong on hardware in turn. Answering everything let
 * three hundred manufactured commands out onto the wire, and the host reported
 * every octet of the answers as an unknown H:4 type with the answer it wanted
 * among them. Answering nothing threw away real Resets. The opcode is what
 * separates them, and only the layer holding the table can look.
 */
static void TestSuspectFilterKeepsRealCommands(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportSetSuspectFilter(&transport, OnlyReset, nullptr);
    HciIntrfTransportOpen(&transport);

    /*
     * Text, then a command the table does not know, then the Reset. All three
     * in one burst, so every packet out of it is suspect and the filter is the
     * only thing that can tell them apart.
     */
    const uint8_t burst[] = {
        'T', 'F', '-', 'M', '\r', '\n',
        0x01, 0x99, 0x99, 0x00,
        0x01, 0x03, 0x0C, 0x00,
    };
    FeedRx(burst, sizeof(burst));
    HciIntrfTransportProcess(&transport);

    assert(transport.DroppedPacketCount == 1U);

    assert(capture.Count == 1U);
    assert(capture.Data[0] == 0x03U && capture.Data[1] == 0x0CU);
    assert(transport.RxPacketCount == 1U);
    assert(transport.SuspectPacketCount == 1U);

    /* Only the delivered one is recorded, and it is marked where it came from. */
    assert(transport.PktMarkLen == 1U);
    assert(transport.PktMark[0].Head[0] == 0x03U);
    assert(transport.PktMark[0].Suspect);

    /* With no filter set, nothing is refused. */
    ResetIntrf();
    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;
    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);
    FeedRx(burst, sizeof(burst));
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 2U);
    assert(transport.DroppedPacketCount == 0U);

    printf("[ok] a suspect burst keeps the commands and loses the rest\n");
}

/*
 * Every packet is recorded, with whether the stream it came from was suspect.
 *
 * The label is what a reader needs and the packet is what the host needs, so
 * both survive. When this refused suspect packets instead, the record was the
 * only thing that showed it was refusing real HCI Resets, which is how that
 * rule came to be taken out.
 */
static void TestPacketMarksRecordBothOutcomes(void)
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

    /* Text, then a Reset with no gap in front of it, so it is refused. */
    const uint8_t noGap[] = {'x', 'y', 0x01, 0x03, 0x0C, 0x00};
    FeedRx(noGap, sizeof(noGap));
    HciIntrfTransportProcess(&transport);

    assert(transport.PktMarkLen == 1U);
    assert(transport.PktMark[0].Type == HCI_H4_PACKET_COMMAND);
    assert(transport.PktMark[0].Head[0] == 0x03);
    assert(transport.PktMark[0].Head[1] == 0x0C);
    assert(transport.PktMark[0].Suspect);
    assert(transport.RxPacketCount == 1U);

    /* The gap, then the same Reset, which is taken. */
    HciIntrfTransportIdle(&transport);
    const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);

    assert(transport.PktMarkLen == 2U);
    assert(transport.PktMark[1].Head[0] == 0x03);
    assert(transport.PktMark[1].Head[1] == 0x0C);
    assert(!transport.PktMark[1].Suspect);

    /*
     * A packet the handler refuses is offered again, and is recorded when it
     * is finally taken rather than once per attempt. Counting on every attempt
     * was the first version of this and inflated the number a reader trusts
     * most.
     */
    capture.Accept = false;
    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);
    assert(transport.PktMarkLen == 2U);

    capture.Accept = true;
    HciIntrfTransportProcess(&transport);
    assert(transport.PktMarkLen == 3U);
    assert(transport.RxPacketCount == 3U);

    printf("[ok] packets are recorded once, with where they came from\n");
}

/*
 * Opening throws away what the port buffered while nobody was listening.
 *
 * This is the case that cost the Thingy:91 its Reset. The driver's buffer
 * fills from the moment the UART is configured, several hundred milliseconds
 * before the controller behind it can answer, so the first read after opening
 * returns octets that were spread across that whole time with every gap
 * between them gone. The bootloader banner and the Reset were four hundred
 * milliseconds apart on the wire and adjacent in the buffer, and the Reset was
 * refused as part of the banner's burst.
 *
 * A gap is the only thing that separates one sender's output from another's,
 * so a buffer that erases gaps has to be emptied rather than read.
 */
static void TestOpenDiscardsWhatArrivedTooEarly(void)
{
    ResetIntrf();

    HciIntrfTransport_t transport;
    Capture capture;
    uint8_t packet[64];

    memset(&capture, 0, sizeof(capture));
    capture.Accept = true;

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));

    /* Banner and a Reset, buffered together while the port was shut. */
    const uint8_t buffered[] = {
        'B', 'o', 'o', 't', 'i', 'n', 'g', ' ', 'T', 'F', '-', 'M', '\r', '\n',
        0x01, 0x03, 0x0C, 0x00,
    };
    FeedRx(buffered, sizeof(buffered));

    HciIntrfTransportOpen(&transport);
    assert(transport.FlushedOctetCount == sizeof(buffered));

    HciIntrfTransportProcess(&transport);
    assert(transport.RxOctetCount == 0U);
    assert(transport.PktMarkLen == 0U);
    assert(!HciIntrfTransportSuspect(&transport));

    /* What the host sends once somebody is listening is taken. */
    const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
    FeedRx(reset, sizeof(reset));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(transport.RxPacketCount == 1U);
    assert(transport.SuspectPacketCount == 0U);
    assert(transport.PktMarkLen == 1U);
    assert(!transport.PktMark[0].Suspect);

    printf("[ok] opening empties the buffer rather than reading it\n");
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

    /*
     * Opening throws away what arrived while it was shut rather than reading
     * it, so this command is gone and not merely delayed. That is the point of
     * the flush and it is checked here as well as in its own case, because
     * this test used to assert the opposite and was the only thing that
     * noticed the behaviour had changed.
     */
    HciIntrfTransportOpen(&transport);
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 0U);
    assert(transport.FlushedOctetCount == sizeof(wire));

    /* What arrives once it is open is read normally. */
    FeedRx(wire, sizeof(wire));
    HciIntrfTransportProcess(&transport);
    assert(capture.Count == 1U);

    HciIntrfTransportClose(&transport);
    assert(!HciIntrfTransportSend(&transport, HCI_H4_PACKET_EVENT, wire, 4U));
    printf("[ok] a closed port reads nothing, sends nothing, keeps nothing\n");
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
    TestFirstOctetsAreKept();
    TestNonH4StreamIsSeenAsSuch();
    TestBannerThenIdleThenCommand();
    TestTextBuiltPacketsAreLabelled();
    TestSuspectFilterKeepsRealCommands();
    TestPacketMarksRecordBothOutcomes();
    TestOpenDiscardsWhatArrivedTooEarly();
    TestSplitReads();
    TestBackpressure();
    TestSendDrains();
    TestSendOversize();
    TestClosedPort();
    TestInitGuards();

    printf("All HCI transport tests passed.\n");
    return 0;
}

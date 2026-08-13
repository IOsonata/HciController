/*
 * Focused regressions for H:4 transport recovery and suspect filtering.
 */

#include "hci_intrf_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_RX_SIZE 256

struct FakeIntrf
{
    DevIntrf_t Base;
    uint8_t RxData[FAKE_RX_SIZE];
    int RxLen;
    int RxOffset;
};

static FakeIntrf gIntrf;

int DeviceIntrfRx(DevIntrf_t *, uint32_t, uint8_t *pBuffer, int BufferLen)
{
    const int available = gIntrf.RxLen - gIntrf.RxOffset;
    if (available <= 0 || BufferLen <= 0)
    {
        return 0;
    }

    const int count = available < BufferLen ? available : BufferLen;
    memcpy(pBuffer, &gIntrf.RxData[gIntrf.RxOffset], (size_t)count);
    gIntrf.RxOffset += count;
    return count;
}

int DeviceIntrfTx(DevIntrf_t *, uint32_t, const uint8_t *, int DataLen)
{
    return DataLen;
}

void DeviceIntrfEnable(DevIntrf_t *) {}
void DeviceIntrfDisable(DevIntrf_t *) {}

struct Capture
{
    unsigned Count;
    HciH4PacketType_t Type;
    size_t Len;
    uint8_t Data[64];
};

static bool CapturePacket(void *pContext,
                          HciH4PacketType_t Type,
                          const uint8_t *pPacket,
                          size_t PacketLen)
{
    Capture *pCapture = static_cast<Capture *>(pContext);
    assert(PacketLen <= sizeof(pCapture->Data));
    pCapture->Count++;
    pCapture->Type = Type;
    pCapture->Len = PacketLen;
    memcpy(pCapture->Data, pPacket, PacketLen);
    return true;
}

/* Same policy shape used by hci_app: only one known command is accepted. */
static bool OnlyReset(void *, HciH4PacketType_t Type,
                      const uint8_t *pPacket, size_t PacketLen)
{
    return Type == HCI_H4_PACKET_COMMAND && PacketLen >= 3U &&
           pPacket[0] == 0x03U && pPacket[1] == 0x0CU &&
           pPacket[2] == 0x00U;
}

static void ResetFake(void)
{
    memset(&gIntrf, 0, sizeof(gIntrf));
}

static void Feed(const uint8_t *pData, size_t Len)
{
    assert(gIntrf.RxLen + (int)Len <= FAKE_RX_SIZE);
    memcpy(&gIntrf.RxData[gIntrf.RxLen], pData, Len);
    gIntrf.RxLen += (int)Len;
}

static void TestCleanPartialAclSurvivesIdle(void)
{
    ResetFake();

    HciIntrfTransport_t transport;
    Capture capture = {};
    uint8_t packet[64];

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportOpen(&transport);

    /*
     * A complete, valid ACL header announces a 32-octet payload and then the
     * sender pauses. This is a clean H:4 stream: no rejected indicator exists
     * to justify abandoning the packet at the gap.
     */
    const uint8_t header[] = {0x02, 0x01, 0x00, 0x20, 0x00};
    Feed(header, sizeof(header));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 0U);
    assert(HciH4ParserIsMidPacket(&transport.Parser));
    assert(!HciIntrfTransportSuspect(&transport));

    HciIntrfTransportIdle(&transport);

    assert(HciH4ParserIsMidPacket(&transport.Parser));
    assert(transport.ResyncCount == 0U);

    /*
     * The remaining payload deliberately begins with a complete HCI_Reset H:4
     * sequence. It must stay ACL payload rather than becoming a command merely
     * because a gap occurred before it.
     */
    uint8_t payload[32] = {0x01, 0x03, 0x0C, 0x00};
    for (size_t i = 4U; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(0x80U + i);
    }

    Feed(payload, sizeof(payload));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(capture.Type == HCI_H4_PACKET_ACL);
    assert(capture.Len == 36U);
    assert(capture.Data[4] == 0x01U);
    assert(capture.Data[5] == 0x03U);
    assert(capture.Data[6] == 0x0CU);
    assert(capture.Data[7] == 0x00U);
    assert(transport.ResyncCount == 0U);

    printf("[ok] idle does not reinterpret clean ACL payload as HCI commands\n");
}

static void TestSuspectDataBypassesCommandFilter(void)
{
    ResetFake();

    HciIntrfTransport_t transport;
    Capture capture = {};
    uint8_t packet[64];

    assert(HciIntrfTransportInit(&transport, &gIntrf.Base, packet,
                                 sizeof(packet), CapturePacket, &capture));
    HciIntrfTransportSetSuspectFilter(&transport, OnlyReset, NULL);
    HciIntrfTransportOpen(&transport);

    /*
     * Two foreign octets make the burst suspect, followed by a legitimate ACL
     * packet. The command filter has no opcode with which to judge ACL data and
     * must not be allowed to discard it. Dropping it here would also lose the
     * host buffer credit spent when the packet was transmitted.
     */
    const uint8_t burst[] = {
        'x', 'y',
        0x02, 0x01, 0x00, 0x00, 0x00,
    };
    Feed(burst, sizeof(burst));
    HciIntrfTransportProcess(&transport);

    assert(capture.Count == 1U);
    assert(capture.Type == HCI_H4_PACKET_ACL);
    assert(capture.Len == 4U);
    assert(transport.DroppedPacketCount == 0U);
    assert(transport.RxPacketCount == 1U);
    assert(transport.SuspectPacketCount == 1U);
    assert(transport.PktMarkLen == 1U);
    assert(transport.PktMark[0].Suspect);

    printf("[ok] suspect ACL data bypasses the opcode-only command filter\n");
}

int main(void)
{
    TestCleanPartialAclSurvivesIdle();
    TestSuspectDataBypassesCommandFilter();
    printf("All HCI transport integrity tests passed.\n");
    return 0;
}

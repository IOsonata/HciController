/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_controller.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_RX_SIZE 512
#define FAKE_TX_SIZE 2048
#define FAKE_QUEUE_DEPTH 4
#define FAKE_PACKET_SIZE (HCI_INTRF_TX_STREAM_SIZE + 16U)

struct FakeIntrf
{
    DevIntrf_t Base;

    uint8_t RxData[FAKE_RX_SIZE];
    int RxLen;
    int RxOffset;

    uint8_t TxData[FAKE_TX_SIZE];
    int TxLen;
    int TxChunkLimit;

    bool PacketMode;
    bool PacketPending;
    HciH4PacketType_t PacketType;
    uint8_t PacketData[128];
    int PacketLen;
    uint32_t LastTxAddr;

    unsigned EnableCount;
    unsigned DisableCount;
};

static FakeIntrf gIntrf;

int DeviceIntrfRx(DevIntrf_t *pDev,
                  uint32_t DevAddr,
                  uint8_t *pBuffer,
                  int BufferLen)
{
    if (pDev != &gIntrf.Base)
    {
        if (!DeviceIntrfStartRx(pDev, DevAddr))
        {
            return 0;
        }
        const int result = DeviceIntrfRxData(pDev, pBuffer, BufferLen);
        if (result >= 0)
        {
            DeviceIntrfStopRx(pDev);
        }
        return result;
    }

    if (gIntrf.PacketMode)
    {
        if (!gIntrf.PacketPending || DevAddr != (uint32_t)gIntrf.PacketType ||
            BufferLen < gIntrf.PacketLen)
        {
            return 0;
        }

        memcpy(pBuffer, gIntrf.PacketData, (size_t)gIntrf.PacketLen);
        const int len = gIntrf.PacketLen;
        gIntrf.PacketPending = false;
        return len;
    }

    int available = gIntrf.RxLen - gIntrf.RxOffset;
    if (available <= 0 || BufferLen <= 0)
    {
        return 0;
    }

    int count = available < BufferLen ? available : BufferLen;
    memcpy(pBuffer, &gIntrf.RxData[gIntrf.RxOffset], (size_t)count);
    gIntrf.RxOffset += count;
    return count;
}

int DeviceIntrfTx(DevIntrf_t *pDev,
                  uint32_t DevAddr,
                  const uint8_t *pData,
                  int DataLen)
{
    if (pDev != &gIntrf.Base)
    {
        if (!DeviceIntrfStartTx(pDev, DevAddr))
        {
            return 0;
        }
        const int result = DeviceIntrfTxData(pDev, pData, DataLen);
        if (result >= 0)
        {
            DeviceIntrfStopTx(pDev);
        }
        return result;
    }

    if (DataLen <= 0)
    {
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
    gIntrf.LastTxAddr = DevAddr;
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

struct FakeController
{
    unsigned PutCount;
    bool PutAccept;
    HciH4PacketType_t PutType[FAKE_QUEUE_DEPTH];
    size_t PutLen[FAKE_QUEUE_DEPTH];
    uint8_t PutData[FAKE_QUEUE_DEPTH][32];

    unsigned GetCount;
    unsigned GetIndex;
    unsigned Pending;
    HciH4PacketType_t GetType[FAKE_QUEUE_DEPTH];
    size_t GetLen[FAKE_QUEUE_DEPTH];
    uint8_t GetData[FAKE_QUEUE_DEPTH][FAKE_PACKET_SIZE];
    HciControllerGetResult_t GetOverride;

    unsigned ProcessCount;
};

static FakeController gController;

static bool FakePut(void *, HciH4PacketType_t Type, const uint8_t *pPacket,
                    size_t PacketLen)
{
    if (!gController.PutAccept)
    {
        return false;
    }

    assert(gController.PutCount < FAKE_QUEUE_DEPTH);
    assert(PacketLen <= sizeof(gController.PutData[0]));

    unsigned index = gController.PutCount++;
    gController.PutType[index] = Type;
    gController.PutLen[index] = PacketLen;
    memcpy(gController.PutData[index], pPacket, PacketLen);
    return true;
}

static HciControllerGetResult_t FakeGet(void *, HciH4PacketType_t *pType,
                                        uint8_t *pPacket,
                                        size_t PacketCapacity,
                                        size_t *pPacketLen)
{
    gController.GetCount++;

    if (gController.GetOverride != HCI_CONTROLLER_GET_EMPTY)
    {
        HciControllerGetResult_t result = gController.GetOverride;
        gController.GetOverride = HCI_CONTROLLER_GET_EMPTY;
        return result;
    }

    if (gController.GetIndex >= gController.Pending)
    {
        return HCI_CONTROLLER_GET_EMPTY;
    }

    unsigned index = gController.GetIndex++;
    size_t len = gController.GetLen[index];
    assert(len <= PacketCapacity);

    *pType = gController.GetType[index];
    memcpy(pPacket, gController.GetData[index], len);
    *pPacketLen = len;
    return HCI_CONTROLLER_GET_PACKET;
}

static void FakeProcess(void *)
{
    gController.ProcessCount++;
}

static const HciControllerOps_t gOps = {
    FakePut,
    FakeGet,
    FakeProcess,
    NULL,
};

struct Fixture
{
    HciController_t Controller;
    uint8_t HostPacket[64];
    uint8_t ControllerPacket[FAKE_PACKET_SIZE];
};

static void ResetFixture(Fixture *pFixture)
{
    memset(&gIntrf, 0, sizeof(gIntrf));
    memset(&gController, 0, sizeof(gController));
    memset(pFixture, 0, sizeof(*pFixture));
    gController.PutAccept = true;
    gController.GetOverride = HCI_CONTROLLER_GET_EMPTY;
}

static void SetupClosed(Fixture *pFixture)
{
    ResetFixture(pFixture);
    assert(HciControllerInit(&pFixture->Controller, &gIntrf.Base,
                             pFixture->HostPacket,
                             sizeof(pFixture->HostPacket),
                             pFixture->ControllerPacket,
                             sizeof(pFixture->ControllerPacket),
                             &gOps));
}

static void Setup(Fixture *pFixture)
{
    SetupClosed(pFixture);
    HciControllerPortOpen(&pFixture->Controller);
}

static void SetupPacket(Fixture *pFixture)
{
    ResetFixture(pFixture);
    gIntrf.PacketMode = true;
    assert(HciControllerInitPacketTransport(&pFixture->Controller,
                                            &gIntrf.Base,
                                            pFixture->HostPacket,
                                            sizeof(pFixture->HostPacket),
                                            pFixture->ControllerPacket,
                                            sizeof(pFixture->ControllerPacket),
                                            &gOps));
    HciControllerPortOpen(&pFixture->Controller);
}

static void FeedHost(const uint8_t *pData, size_t Len)
{
    assert(gIntrf.RxLen + (int)Len <= FAKE_RX_SIZE);
    memcpy(&gIntrf.RxData[gIntrf.RxLen], pData, Len);
    gIntrf.RxLen += (int)Len;
}

static void FeedPacket(HciH4PacketType_t Type, const uint8_t *pData, size_t Len)
{
    assert(gIntrf.PacketMode && !gIntrf.PacketPending);
    assert(Len <= sizeof(gIntrf.PacketData));
    memcpy(gIntrf.PacketData, pData, Len);
    gIntrf.PacketType = Type;
    gIntrf.PacketLen = (int)Len;
    gIntrf.PacketPending = true;
}

static void QueueController(HciH4PacketType_t Type,
                            const uint8_t *pData,
                            size_t Len)
{
    assert(gController.Pending < FAKE_QUEUE_DEPTH);
    assert(Len <= sizeof(gController.GetData[0]));

    unsigned index = gController.Pending++;
    gController.GetType[index] = Type;
    gController.GetLen[index] = Len;
    memcpy(gController.GetData[index], pData, Len);
}

static void TestHostToController(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedHost(wire, sizeof(wire));
    HciControllerProcess(&fixture.Controller);

    assert(gController.PutCount == 1U);
    assert(gController.PutType[0] == HCI_H4_PACKET_COMMAND);
    assert(gController.PutLen[0] == 3U);
    assert(gController.PutData[0][0] == 0x03);
    printf("[ok] H4 host command reaches the controller through packet DeviceIntrf\n");
}

static void TestPreopenH4BacklogIsPreserved(void)
{
    Fixture fixture;
    SetupClosed(&fixture);

    const uint8_t reset[] = {0x01, 0x03, 0x0C, 0x00};
    FeedHost(reset, sizeof(reset));

    HciControllerPortOpen(&fixture.Controller);
    assert(fixture.Controller.Host.FlushedOctetCount == 0U);
    assert(fixture.Controller.Host.RxOctetCount == sizeof(reset));

    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);
    assert(gController.PutType[0] == HCI_H4_PACKET_COMMAND);
    printf("[ok] a clean H4 Reset queued before open is preserved\n");
}

static void TestPreopenMixedBacklogIsFlushed(void)
{
    Fixture fixture;
    SetupClosed(&fixture);

    const uint8_t buffered[] = {
        'B', 'o', 'o', 't', 'i', 'n', 'g', ' ', 'T', 'F', '-', 'M', '\r', '\n',
        0x01, 0x03, 0x0C, 0x00,
    };
    FeedHost(buffered, sizeof(buffered));

    HciControllerPortOpen(&fixture.Controller);
    HciControllerProcess(&fixture.Controller);

    assert(gController.PutCount == 0U);
    assert(fixture.Controller.Host.FlushedOctetCount == sizeof(buffered));
    printf("[ok] ordinary H4 still flushes a text-prefixed pre-open backlog\n");
}

static void TestUartStartupResetBacklogIsRecovered(void)
{
    Fixture fixture;
    SetupClosed(&fixture);
    HciControllerSetH4StartupResetSync(&fixture.Controller, true);

    const uint8_t buffered[] = {
        'B', 'o', 'o', 't', 'i', 'n', 'g', ' ', 'T', 'F', '-', 'M', '\r', '\n',
        0x01, 0x03, 0x0C, 0x00,
    };
    FeedHost(buffered, sizeof(buffered));

    HciControllerPortOpen(&fixture.Controller);
    HciControllerProcess(&fixture.Controller);

    assert(gController.PutCount == 1U);
    assert(gController.PutType[0] == HCI_H4_PACKET_COMMAND);
    assert(gController.PutLen[0] == 3U);
    assert(gController.PutData[0][0] == 0x03U);
    assert(gController.PutData[0][1] == 0x0CU);
    assert(gController.PutData[0][2] == 0x00U);
    assert(fixture.Controller.Host.FlushedOctetCount == sizeof(buffered) - 4U);
    assert(fixture.Controller.Host.RxOctetCount == sizeof(buffered));
    printf("[ok] UART startup scan discards boot text and preserves HCI Reset\n");
}

static void TestUartStartupResetAcrossReadsIsRecovered(void)
{
    Fixture fixture;
    SetupClosed(&fixture);
    HciControllerSetH4StartupResetSync(&fixture.Controller, true);

    uint8_t buffered[HCI_INTRF_IO_CHUNK_SIZE + 3U];
    memset(buffered, 'X', sizeof(buffered));
    buffered[HCI_INTRF_IO_CHUNK_SIZE - 1U] = 0x01U;
    buffered[HCI_INTRF_IO_CHUNK_SIZE] = 0x03U;
    buffered[HCI_INTRF_IO_CHUNK_SIZE + 1U] = 0x0CU;
    buffered[HCI_INTRF_IO_CHUNK_SIZE + 2U] = 0x00U;
    FeedHost(buffered, sizeof(buffered));

    HciControllerPortOpen(&fixture.Controller);
    HciControllerProcess(&fixture.Controller);

    assert(gController.PutCount == 1U);
    assert(gController.PutType[0] == HCI_H4_PACKET_COMMAND);
    assert(gController.PutLen[0] == 3U);
    assert(gController.PutData[0][0] == 0x03U);
    assert(gController.PutData[0][1] == 0x0CU);
    assert(gController.PutData[0][2] == 0x00U);
    assert(fixture.Controller.Host.FlushedOctetCount ==
           HCI_INTRF_IO_CHUNK_SIZE - 1U);
    assert(fixture.Controller.Host.RxOctetCount == sizeof(buffered));
    printf("[ok] UART startup Reset scan survives an RX chunk boundary\n");
}

static void TestHostRetry(void)
{
    Fixture fixture;
    Setup(&fixture);
    gController.PutAccept = false;

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedHost(wire, sizeof(wire));

    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 0U);
    assert(fixture.Controller.HostPacketPending);

    gController.PutAccept = true;
    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);
    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);
    printf("[ok] refused H4 packet is retained above the packet DeviceIntrf\n");
}

static void TestControllerToHost(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t event[] = {0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);

    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    assert(gIntrf.TxData[0] == HCI_H4_PACKET_EVENT);
    assert(memcmp(&gIntrf.TxData[1], event, sizeof(event)) == 0);
    printf("[ok] packet DeviceIntrf adapter adds H4 only on the UART/CDC wire\n");
}

static void TestControllerOrdering(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t first[] = {0x0E, 0x01, 0x11};
    const uint8_t second[] = {0x0F, 0x01, 0x22};
    QueueController(HCI_H4_PACKET_EVENT, first, sizeof(first));
    QueueController(HCI_H4_PACKET_EVENT, second, sizeof(second));

    HciControllerProcess(&fixture.Controller);
    HciControllerProcess(&fixture.Controller);

    assert(gIntrf.TxLen == (int)(sizeof(first) + sizeof(second) + 2U));
    assert(gIntrf.TxData[1] == 0x0E && gIntrf.TxData[3] == 0x11);
    assert(gIntrf.TxData[5] == 0x0F && gIntrf.TxData[7] == 0x22);
    printf("[ok] queued controller events keep their order\n");
}

static void TestPacketDeviceIntrf(void)
{
    Fixture fixture;
    SetupPacket(&fixture);
    assert(gIntrf.EnableCount == 1U);

    const uint8_t reset[] = {0x03U, 0x0CU, 0x00U};
    FeedPacket(HCI_H4_PACKET_COMMAND, reset, sizeof(reset));
    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);
    assert(gController.PutType[0] == HCI_H4_PACKET_COMMAND);
    assert(gController.PutLen[0] == sizeof(reset));
    assert(memcmp(gController.PutData[0], reset, sizeof(reset)) == 0);

    const uint8_t event[] = {0x0EU, 0x01U, 0x00U};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);
    assert(gIntrf.LastTxAddr == HCI_H4_PACKET_EVENT);
    assert(gIntrf.TxLen == (int)sizeof(event));
    assert(memcmp(gIntrf.TxData, event, sizeof(event)) == 0);

    const uint8_t outputAcl[] = {0x01U, 0x20U, 0x00U, 0x00U};
    QueueController(HCI_H4_PACKET_ACL, outputAcl, sizeof(outputAcl));
    HciControllerProcess(&fixture.Controller);
    assert(fixture.Controller.ControllerAclPacketCount == 1U);
    assert(fixture.Controller.HostAclPacketCount == 1U);
    assert(gIntrf.LastTxAddr == HCI_H4_PACKET_ACL);

    gController.PutAccept = false;
    const uint8_t acl[] = {0x01U, 0x00U, 0x00U, 0x00U};
    FeedPacket(HCI_H4_PACKET_ACL, acl, sizeof(acl));
    HciControllerProcess(&fixture.Controller);
    assert(!gIntrf.PacketPending);
    assert(fixture.Controller.HostPacketPending);

    gController.PutAccept = true;
    HciControllerProcess(&fixture.Controller);
    assert(!fixture.Controller.HostPacketPending);
    assert(gController.PutCount == 2U);
    assert(gController.PutType[1] == HCI_H4_PACKET_ACL);

    HciControllerPortClose(&fixture.Controller);
    assert(gIntrf.DisableCount == 1U);
    printf("[ok] native packet DeviceIntrf is swappable with the H4 adapter\n");
}

static void TestControllerError(void)
{
    Fixture fixture;
    Setup(&fixture);

    gController.GetOverride = HCI_CONTROLLER_GET_ERROR;
    HciControllerProcess(&fixture.Controller);
    assert(fixture.Controller.ControllerGetErrorCount == 1U);
    assert(gIntrf.TxLen == 0);

    const uint8_t event[] = {0x0E, 0x01, 0x33};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);
    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    printf("[ok] controller get error is counted, bridge keeps running\n");
}

static void TestInvalidControllerType(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t command[] = {0x03, 0x0C, 0x00};
    QueueController(HCI_H4_PACKET_COMMAND, command, sizeof(command));
    HciControllerProcess(&fixture.Controller);

    assert(fixture.Controller.InvalidControllerPacketCount == 1U);
    assert(gIntrf.TxLen == 0);
    printf("[ok] a command indicator from the controller is rejected\n");
}

static void TestZeroLengthRejected(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t nothing[] = {0x00};
    QueueController(HCI_H4_PACKET_EVENT, nothing, 0U);
    HciControllerProcess(&fixture.Controller);

    assert(fixture.Controller.InvalidControllerPacketCount == 1U);
    assert(gIntrf.TxLen == 0);

    const uint8_t event[] = {0x0E, 0x01, 0x44};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);
    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    printf("[ok] a zero length controller packet is rejected\n");
}

static void TestUnsendableDropped(void)
{
    Fixture fixture;
    Setup(&fixture);

    static uint8_t big[FAKE_PACKET_SIZE];
    memset(big, 0xA5, sizeof(big));
    QueueController(HCI_H4_PACKET_ACL, big, HCI_INTRF_TX_STREAM_SIZE);
    HciControllerProcess(&fixture.Controller);

    assert(fixture.Controller.UnsendableControllerPacketCount == 1U);
    assert(!fixture.Controller.ControllerPacketPending);
    assert(gIntrf.TxLen == 0);

    const uint8_t event[] = {0x0E, 0x01, 0x55};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);
    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    printf("[ok] an unsendable controller packet is dropped, not wedged\n");
}

static void TestInitGuards(void)
{
    HciController_t controller;
    uint8_t host[64];
    uint8_t ctlr[64];
    HciControllerOps_t ops = gOps;

    memset(&gIntrf, 0, sizeof(gIntrf));

    assert(!HciControllerInit(NULL, &gIntrf.Base, host, sizeof(host), ctlr,
                              sizeof(ctlr), &ops));
    assert(!HciControllerInit(&controller, NULL, host, sizeof(host), ctlr,
                              sizeof(ctlr), &ops));
    assert(!HciControllerInit(&controller, &gIntrf.Base, host, 0U, ctlr,
                              sizeof(ctlr), &ops));
    assert(!HciControllerInit(&controller, &gIntrf.Base, host, sizeof(host),
                              NULL, sizeof(ctlr), &ops));
    assert(!HciControllerInit(&controller, &gIntrf.Base, host, sizeof(host),
                              ctlr, sizeof(ctlr), NULL));

    ops.Put = NULL;
    assert(!HciControllerInit(&controller, &gIntrf.Base, host, sizeof(host),
                              ctlr, sizeof(ctlr), &ops));
    ops = gOps;
    ops.Get = NULL;
    assert(!HciControllerInit(&controller, &gIntrf.Base, host, sizeof(host),
                              ctlr, sizeof(ctlr), &ops));
    printf("[ok] init rejects unusable arguments\n");
}

int main(void)
{
    TestHostToController();
    TestPreopenH4BacklogIsPreserved();
    TestPreopenMixedBacklogIsFlushed();
    TestUartStartupResetBacklogIsRecovered();
    TestUartStartupResetAcrossReadsIsRecovered();
    TestHostRetry();
    TestControllerToHost();
    TestControllerOrdering();
    TestPacketDeviceIntrf();
    TestControllerError();
    TestInvalidControllerType();
    TestZeroLengthRejected();
    TestUnsendableDropped();
    TestInitGuards();

    printf("All HCI controller bridge tests passed.\n");
    return 0;
}

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
 * Host test for the bridge between the host transport and the controller ops.
 *
 * The controller side is a table driven fake, the host side is the same
 * DevIntrf fake used by the transport test. Together they cover the two
 * directions and the backpressure retry in each.
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

    int count = DataLen;
    if (gIntrf.TxChunkLimit > 0 && count > gIntrf.TxChunkLimit)
    {
        count = gIntrf.TxChunkLimit;
    }

    assert(gIntrf.TxLen + count <= FAKE_TX_SIZE);
    memcpy(&gIntrf.TxData[gIntrf.TxLen], pData, (size_t)count);
    gIntrf.TxLen += count;
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
    /* Host to controller. */
    unsigned PutCount;
    bool PutAccept;
    HciH4PacketType_t PutType[FAKE_QUEUE_DEPTH];
    size_t PutLen[FAKE_QUEUE_DEPTH];
    uint8_t PutData[FAKE_QUEUE_DEPTH][32];

    /* Controller to host. */
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

static void SetupClosed(Fixture *pFixture)
{
    memset(&gIntrf, 0, sizeof(gIntrf));
    memset(&gController, 0, sizeof(gController));
    memset(pFixture, 0, sizeof(*pFixture));

    gController.PutAccept = true;
    gController.GetOverride = HCI_CONTROLLER_GET_EMPTY;

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

static void FeedHost(const uint8_t *pData, size_t Len)
{
    assert(gIntrf.RxLen + (int)Len <= FAKE_RX_SIZE);
    memcpy(&gIntrf.RxData[gIntrf.RxLen], pData, Len);
    gIntrf.RxLen += (int)Len;
}

static void QueueController(HciH4PacketType_t Type, const uint8_t *pData,
                            size_t Len)
{
    assert(gController.Pending < FAKE_QUEUE_DEPTH);
    assert(Len <= sizeof(gController.GetData[0]));

    unsigned index = gController.Pending++;
    gController.GetType[index] = Type;
    gController.GetLen[index] = Len;
    memcpy(gController.GetData[index], pData, Len);
}

/* A command from the host reaches the controller with the indicator stripped. */
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
    assert(gController.ProcessCount >= 1U);
    printf("[ok] host command reaches the controller\n");
}

/*
 * A clean host may send Reset while the controller is still coming up. H:4
 * has no transport retry, so a complete packet already queued when the port
 * opens must survive that boundary.
 */
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
    assert(gController.PutLen[0] == 3U);
    assert(gController.PutData[0][0] == 0x03U);
    assert(gController.PutData[0][1] == 0x0CU);
    printf("[ok] a clean H4 Reset queued before open is preserved\n");
}

/*
 * The Thingy:91 case is the opposite: the same FIFO begins with boot text and
 * only later contains H:4. The UART buffer erased the real time gap, so the
 * whole stale mixed backlog still has to be discarded at open.
 */
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
    assert(fixture.Controller.Host.RxOctetCount == 0U);
    printf("[ok] a text-prefixed pre-open backlog is still flushed\n");
}

/* A controller refusing the packet gets it again, and it is not duplicated. */
static void TestHostRetry(void)
{
    Fixture fixture;
    Setup(&fixture);

    gController.PutAccept = false;

    const uint8_t wire[] = {0x01, 0x03, 0x0C, 0x00};
    FeedHost(wire, sizeof(wire));

    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 0U);

    gController.PutAccept = true;
    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);

    HciControllerProcess(&fixture.Controller);
    assert(gController.PutCount == 1U);
    printf("[ok] refused host packet is retried exactly once\n");
}

/* An event from the controller reaches the wire with the indicator prefixed. */
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
    printf("[ok] controller event reaches the host wire\n");
}

/* Two queued events go out in order across successive passes. */
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

/* A controller side error is counted and does not stall the bridge. */
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

/* An indicator the controller must never emit is rejected. */
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

/* A zero length packet would put a bare indicator on the wire. */
static void TestZeroLengthRejected(void)
{
    Fixture fixture;
    Setup(&fixture);

    const uint8_t nothing[] = {0x00};
    QueueController(HCI_H4_PACKET_EVENT, nothing, 0U);

    HciControllerProcess(&fixture.Controller);

    assert(fixture.Controller.InvalidControllerPacketCount == 1U);
    assert(gIntrf.TxLen == 0);

    /* The bridge keeps running behind it. */
    const uint8_t event[] = {0x0E, 0x01, 0x44};
    QueueController(HCI_H4_PACKET_EVENT, event, sizeof(event));
    HciControllerProcess(&fixture.Controller);
    assert(gIntrf.TxLen == (int)sizeof(event) + 1);
    printf("[ok] a zero length controller packet is rejected\n");
}

/*
 * A packet the transport can never accept is dropped, not held pending. Held
 * pending it would stop the controller to host direction for good, because no
 * further packet is fetched while one is pending.
 */
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
    assert(gIntrf.TxData[1] == 0x0E && gIntrf.TxData[3] == 0x55);
    printf("[ok] an unsendable controller packet is dropped, not wedged\n");
}

/* Init rejects the argument combinations that cannot work. */
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
    TestHostRetry();
    TestControllerToHost();
    TestControllerOrdering();
    TestControllerError();
    TestInvalidControllerType();
    TestZeroLengthRejected();
    TestUnsendableDropped();
    TestInitGuards();

    printf("All HCI controller bridge tests passed.\n");
    return 0;
}

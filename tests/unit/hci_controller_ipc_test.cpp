/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * HciController with an IpcIntrf host. The controller side is the IPC owner,
 * the host side is the peer, as on the nRF5340: HciControllerInit takes the
 * IpcIntrf the way it takes the UART or the CDC, and H:4 crosses the rings.
 * The controller itself is a fake that records what it is given.
 */

#include "hci_controller.h"
#include "ipc/ipc_intrf.h"

#include <stdio.h>
#include <string.h>

#include "../stubs/ipc_sync_port.h"

#define RING_BYTES 256U
#define MEM_BYTES  IPC_INTRF_MEMSIZE(RING_BYTES, 0U, RING_BYTES, 0U)

static int g_Pass;
static int g_Fail;

#define CHECK(Cond) \
    do { \
        if (Cond) { g_Pass++; } \
        else { g_Fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); } \
    } while (0)

enum { CORE_CTRL = 0, CORE_HOST = 1 };

struct FakeController
{
    unsigned PutCnt;
    HciH4PacketType_t LastType;
    uint8_t LastPacket[1100];
    size_t LastLen;

    uint8_t Out[8][300];
    size_t OutLen[8];
    HciH4PacketType_t OutType[8];
    unsigned OutHead;
    unsigned OutTail;
};

static FakeController g_Ctrl;

static bool FakePut(void *, HciH4PacketType_t Type, const uint8_t *pPacket, size_t Len)
{
    g_Ctrl.PutCnt++;
    g_Ctrl.LastType = Type;
    g_Ctrl.LastLen = Len;
    memcpy(g_Ctrl.LastPacket, pPacket, Len);
    return true;
}

static HciControllerGetResult_t FakeGet(void *, HciH4PacketType_t *pType, uint8_t *pPacket,
                                        size_t Capacity, size_t *pLen)
{
    if (g_Ctrl.OutHead == g_Ctrl.OutTail)
    {
        return HCI_CONTROLLER_GET_EMPTY;
    }

    const unsigned i = g_Ctrl.OutHead % 8U;
    if (g_Ctrl.OutLen[i] > Capacity)
    {
        return HCI_CONTROLLER_GET_ERROR;
    }

    memcpy(pPacket, g_Ctrl.Out[i], g_Ctrl.OutLen[i]);
    *pType = g_Ctrl.OutType[i];
    *pLen = g_Ctrl.OutLen[i];
    g_Ctrl.OutHead++;
    return HCI_CONTROLLER_GET_PACKET;
}

static bool QueueOut(HciH4PacketType_t Type, const uint8_t *pPacket, size_t Len)
{
    if (g_Ctrl.OutTail - g_Ctrl.OutHead >= 8U || Len > sizeof(g_Ctrl.Out[0]))
    {
        return false;
    }

    const unsigned i = g_Ctrl.OutTail % 8U;
    memcpy(g_Ctrl.Out[i], pPacket, Len);
    g_Ctrl.OutLen[i] = Len;
    g_Ctrl.OutType[i] = Type;
    g_Ctrl.OutTail++;
    return true;
}

alignas(8) static uint8_t s_Mem[MEM_BYTES];
static IpcIntrfDev_t s_Owner;
static IpcIntrfDev_t s_Peer;
static HciController_t s_Hci;
static uint8_t s_HostPacket[HCI_INTRF_TX_STREAM_SIZE - 1U];
static uint8_t s_CtrlPacket[HCI_INTRF_TX_STREAM_SIZE - 1U];

static int IpcEvt(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    return Len;
}

static bool Setup(void)
{
    memset(&g_Ctrl, 0, sizeof(g_Ctrl));
    memset(s_Mem, 0xA5, sizeof(s_Mem));

    IpcIntrfCfg_t oc = {};
    oc.pMemBlk = s_Mem;
    oc.MemSize = sizeof(s_Mem);
    oc.bOwner = true;
    oc.TxNbBlk = RING_BYTES;
    oc.RxNbBlk = RING_BYTES;
    oc.TxChan = 0;
    oc.RxChan = 1;
    oc.IntPrio = 6;
    oc.EvtCB = IpcEvt;

    IpcIntrfCfg_t pc = {};
    pc.pMemBlk = s_Mem;
    pc.MemSize = sizeof(s_Mem);
    pc.bOwner = false;
    pc.TxChan = 1;
    pc.RxChan = 0;
    pc.IntPrio = 6;
    pc.EvtCB = IpcEvt;

    if (!IpcIntrfInit(&s_Owner, &oc) || !IpcIntrfInit(&s_Peer, &pc))
    {
        return false;
    }
    s_Owner.DevIntrf.MaxRetry = 1;
    s_Peer.DevIntrf.MaxRetry = 1;

    HciControllerOps_t ops = {};
    ops.Put = FakePut;
    ops.Get = FakeGet;

    if (!HciControllerInit(&s_Hci, &s_Owner.DevIntrf, s_HostPacket, sizeof(s_HostPacket),
                           s_CtrlPacket, sizeof(s_CtrlPacket), &ops))
    {
        return false;
    }

    HciControllerPortOpen(&s_Hci);
    return true;
}

static void Run(unsigned Passes)
{
    for (unsigned i = 0; i < Passes; i++)
    {
        HciControllerProcess(&s_Hci);
    }
}

static int HostTx(const uint8_t *pData, int Len)
{
    return DeviceIntrfTx(&s_Peer.DevIntrf, 0, pData, Len);
}

static int HostRx(uint8_t *pData, int Len)
{
    return DeviceIntrfRx(&s_Peer.DevIntrf, 0, pData, Len);
}

static void TestCommandEvent(void)
{
    printf("command in, event out\n");
    CHECK(Setup());
    CHECK(IpcIntrfIsUp(&s_Peer));

    const uint8_t reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    const uint8_t evt[] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    uint8_t in[32];

    CHECK(HostTx(reset, sizeof(reset)) == (int)sizeof(reset));
    Run(4);
    CHECK(g_Ctrl.PutCnt == 1U);
    CHECK(g_Ctrl.LastType == HCI_H4_PACKET_COMMAND);
    CHECK(g_Ctrl.LastLen == 3U);
    CHECK(memcmp(g_Ctrl.LastPacket, reset + 1, 3) == 0);

    CHECK(QueueOut(HCI_H4_PACKET_EVENT, evt, sizeof(evt)));
    Run(4);
    CHECK(HostRx(in, sizeof(in)) == (int)(sizeof(evt) + 1U));
    CHECK(in[0] == 0x04);
    CHECK(memcmp(in + 1, evt, sizeof(evt)) == 0);
}

static void TestSplitCommand(void)
{
    printf("a command that arrives in pieces\n");
    CHECK(Setup());

    const uint8_t cmd[] = { 0x01, 0x20, 0x20, 0x03, 0x11, 0x22, 0x33 };

    CHECK(HostTx(cmd, 2) == 2);
    Run(3);
    CHECK(g_Ctrl.PutCnt == 0U);
    CHECK(HostTx(cmd + 2, 3) == 3);
    Run(3);
    CHECK(g_Ctrl.PutCnt == 0U);
    CHECK(HostTx(cmd + 5, 2) == 2);
    Run(3);
    CHECK(g_Ctrl.PutCnt == 1U);
    CHECK(g_Ctrl.LastLen == 6U);
    CHECK(memcmp(g_Ctrl.LastPacket, cmd + 1, 6) == 0);
}

static void TestAcl(void)
{
    printf("acl both ways across the ring wrap\n");
    CHECK(Setup());

    for (unsigned round = 0; round < 12; round++)
    {
        uint8_t down[5 + 120];
        down[0] = 0x02;
        down[1] = 0x40;
        down[2] = 0x00;
        down[3] = 120;
        down[4] = 0x00;
        for (unsigned i = 0; i < 120; i++)
        {
            down[5 + i] = (uint8_t)(round * 13U + i);
        }
        CHECK(HostTx(down, sizeof(down)) == (int)sizeof(down));
        Run(4);
        CHECK(g_Ctrl.PutCnt == round + 1U);
        CHECK(g_Ctrl.LastType == HCI_H4_PACKET_ACL);
        CHECK(g_Ctrl.LastLen == sizeof(down) - 1U);
        CHECK(memcmp(g_Ctrl.LastPacket, down + 1, sizeof(down) - 1U) == 0);

        uint8_t up[4 + 100];
        up[0] = 0x40;
        up[1] = 0x00;
        up[2] = 100;
        up[3] = 0x00;
        for (unsigned i = 0; i < 100; i++)
        {
            up[4 + i] = (uint8_t)(round * 7U + i);
        }
        CHECK(QueueOut(HCI_H4_PACKET_ACL, up, sizeof(up)));
        Run(4);

        uint8_t in[200];
        CHECK(HostRx(in, sizeof(in)) == (int)(sizeof(up) + 1U));
        CHECK(in[0] == 0x02);
        CHECK(memcmp(in + 1, up, sizeof(up)) == 0);
    }
}

static void TestFullRing(void)
{
    printf("full ring: nothing lost, order kept\n");
    CHECK(Setup());

    // Eight events of 60 bytes do not fit the 256 byte ring at once.
    uint8_t evt[2 + 58];
    evt[0] = 0xFF;
    evt[1] = 58;
    for (unsigned n = 0; n < 8; n++)
    {
        memset(evt + 2, (int)(0x10 + n), 58);
        CHECK(QueueOut(HCI_H4_PACKET_EVENT, evt, sizeof(evt)));
    }

    // The host reads nothing yet: the controller side keeps a packet pending
    // instead of dropping it.
    Run(40);
    CHECK(s_Hci.UnsendableControllerPacketCount == 0U);

    static uint8_t acc[8 * 61 + 64];
    size_t have = 0;

    for (unsigned pass = 0; pass < 80 && have < 8U * 61U; pass++)
    {
        int n = HostRx(acc + have, 37);

        if (n > 0)
        {
            have += (size_t)n;
        }
        Run(4);
    }

    CHECK(have == 8U * 61U);
    for (unsigned n = 0; n < 8U && have >= (n + 1U) * 61U; n++)
    {
        const uint8_t *p = acc + n * 61U;

        CHECK(p[0] == 0x04);
        CHECK(p[1] == 0xFF);
        CHECK(p[2] == 58);
        CHECK(p[3] == (uint8_t)(0x10 + n));
        CHECK(p[60] == (uint8_t)(0x10 + n));
    }
}

int main(void)
{
    TestCommandEvent();
    TestSplitCommand();
    TestAcl();
    TestFullRing();

    printf("\n%d passed, %d failed\n", g_Pass, g_Fail);
    return g_Fail == 0 ? 0 : 1;
}

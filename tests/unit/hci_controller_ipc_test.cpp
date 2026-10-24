/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * HciController with a BtHciIpc host. The controller side is the IPC owner,
 * the host side is the peer, as on the nRF5340. BtHciIpc is a packet
 * interface like native USB: HciControllerInitPacketTransport takes it, one
 * ring slot is one HCI packet and DevAddr is the packet type. The host side
 * here is a plain IpcIntrf peer that writes and reads slots as the app core
 * does, so the slot format is checked from outside the transport code.
 * The controller itself is a fake that records what it is given.
 */

#include "hci_controller.h"
#include "hci_ctlr_fake_intrf.h"
#include "bluetooth/bt_hci_ipc.h"
#include "ipc/ipc_intrf.h"

#include <stdio.h>
#include <string.h>

#include "../stubs/ipc_sync_port.h"

#define NB_SLOT    4U
#define MEM_BYTES  BT_HCI_IPC_MEMSIZE(NB_SLOT)

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

static HciSdcGetResult_t FakeGet(void *, HciH4PacketType_t *pType, uint8_t *pPacket,
                                        size_t Capacity, size_t *pLen)
{
    if (g_Ctrl.OutHead == g_Ctrl.OutTail)
    {
        return HCI_SDC_GET_EMPTY;
    }

    const unsigned i = g_Ctrl.OutHead % 8U;
    if (g_Ctrl.OutLen[i] > Capacity)
    {
        return HCI_SDC_GET_ERROR;
    }

    memcpy(pPacket, g_Ctrl.Out[i], g_Ctrl.OutLen[i]);
    *pType = g_Ctrl.OutType[i];
    *pLen = g_Ctrl.OutLen[i];
    g_Ctrl.OutHead++;
    return HCI_SDC_GET_PACKET;
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
static BtHciIpcIntrfDev_t s_Owner;
static IpcIntrfDev_t s_Peer;
static HciController_t s_Hci;
static uint8_t s_HostPacket[BT_HCI_IPC_PKT_MAXLEN];
static uint8_t s_CtrlPacket[BT_HCI_IPC_PKT_MAXLEN];
static unsigned s_RxEvt;

static int IpcEvt(DevIntrf_t *, DEVINTRF_EVT Evt, uint8_t *, int Len)
{
    if (Evt == DEVINTRF_EVT_RX_DATA)
    {
        s_RxEvt++;
    }
    return Len;
}

static int PeerEvt(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    return Len;
}

static bool Setup(void)
{
    memset(&g_Ctrl, 0, sizeof(g_Ctrl));
    memset(s_pIpcByRx, 0, sizeof(s_pIpcByRx));
    memset(s_Mem, 0xA5, sizeof(s_Mem));
    s_RxEvt = 0;

    BtHciIpcIntrfCfg_t oc = {};
    oc.pMemBlk = s_Mem;
    oc.MemSize = sizeof(s_Mem);
    oc.bOwner = true;
    oc.NbPkt = NB_SLOT;
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
    pc.EvtCB = PeerEvt;

    if (!BtHciIpcIntrfInit(&s_Owner, &oc) || !IpcIntrfInit(&s_Peer, &pc))
    {
        return false;
    }
    s_Peer.DevIntrf.MaxRetry = 1;

    static HciCtlrFakeIntrf_t fake;


    DevIntrf_t *ctlr = HciCtlrFakeIntrfInit(&fake, FakePut, FakeGet, NULL);

    if (!HciControllerInitPacketTransport(&s_Hci, BtHciIpcIntrfGet(&s_Owner),
                                          s_HostPacket, sizeof(s_HostPacket),
                                          s_CtrlPacket, sizeof(s_CtrlPacket), ctlr))
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

/* One slot: the type byte then the HCI packet. */
static int HostTx(uint8_t Type, const uint8_t *pPkt, int Len)
{
    uint8_t slot[1 + BT_HCI_IPC_PKT_MAXLEN];

    slot[0] = Type;
    memcpy(slot + 1, pPkt, (size_t)Len);
    return DeviceIntrfTx(&s_Peer.DevIntrf, 0, slot, Len + 1);
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

    const uint8_t reset[] = { 0x03, 0x0C, 0x00 };
    const uint8_t evt[] = { 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    uint8_t in[32];

    CHECK(HostTx(0x01, reset, sizeof(reset)) == (int)sizeof(reset) + 1);
    CHECK(s_RxEvt >= 1U);
    Run(4);
    CHECK(g_Ctrl.PutCnt == 1U);
    CHECK(g_Ctrl.LastType == HCI_H4_PACKET_COMMAND);
    CHECK(g_Ctrl.LastLen == 3U);
    CHECK(memcmp(g_Ctrl.LastPacket, reset, 3) == 0);

    CHECK(QueueOut(HCI_H4_PACKET_EVENT, evt, sizeof(evt)));
    Run(4);
    CHECK(HostRx(in, sizeof(in)) == (int)(sizeof(evt) + 1U));
    CHECK(in[0] == 0x04);
    CHECK(memcmp(in + 1, evt, sizeof(evt)) == 0);
}

static void TestBadSlot(void)
{
    printf("a slot that is not a whole packet is dropped\n");
    CHECK(Setup());

    /* Unknown type, then a command whose length byte says 3 but carries 1. */
    const uint8_t junk[] = { 0x11, 0x22, 0x33 };
    const uint8_t shortCmd[] = { 0x20, 0x20, 0x03, 0x11 };
    const uint8_t good[] = { 0x20, 0x20, 0x03, 0x11, 0x22, 0x33 };

    CHECK(HostTx(0x77, junk, sizeof(junk)) == (int)sizeof(junk) + 1);
    CHECK(HostTx(0x01, shortCmd, sizeof(shortCmd)) == (int)sizeof(shortCmd) + 1);
    CHECK(HostTx(0x01, good, sizeof(good)) == (int)sizeof(good) + 1);
    Run(6);
    CHECK(g_Ctrl.PutCnt == 1U);
    CHECK(g_Ctrl.LastLen == sizeof(good));
    CHECK(memcmp(g_Ctrl.LastPacket, good, sizeof(good)) == 0);
    CHECK(s_Owner.SyncErrCnt == 2U);
}

static void TestAcl(void)
{
    printf("acl both ways across the ring wrap\n");
    CHECK(Setup());

    for (unsigned round = 0; round < 12; round++)
    {
        uint8_t down[4 + 120];
        down[0] = 0x40;
        down[1] = 0x00;
        down[2] = 120;
        down[3] = 0x00;
        for (unsigned i = 0; i < 120; i++)
        {
            down[4 + i] = (uint8_t)(round * 13U + i);
        }
        CHECK(HostTx(0x02, down, sizeof(down)) == (int)sizeof(down) + 1);
        Run(4);
        CHECK(g_Ctrl.PutCnt == round + 1U);
        CHECK(g_Ctrl.LastType == HCI_H4_PACKET_ACL);
        CHECK(g_Ctrl.LastLen == sizeof(down));
        CHECK(memcmp(g_Ctrl.LastPacket, down, sizeof(down)) == 0);

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

        uint8_t in[300];
        CHECK(HostRx(in, sizeof(in)) == (int)(sizeof(up) + 1U));
        CHECK(in[0] == 0x02);
        CHECK(memcmp(in + 1, up, sizeof(up)) == 0);
    }
}

static void TestFullRing(void)
{
    printf("full ring: nothing lost, order kept\n");
    CHECK(Setup());

    /* Eight events, four slots: the controller keeps one pending until the
     * host frees a slot instead of dropping it. */
    uint8_t evt[2 + 58];
    evt[0] = 0xFF;
    evt[1] = 58;
    for (unsigned n = 0; n < 8; n++)
    {
        memset(evt + 2, (int)(0x10 + n), 58);
        CHECK(QueueOut(HCI_H4_PACKET_EVENT, evt, sizeof(evt)));
    }

    Run(40);
    CHECK(s_Hci.UnsendableControllerPacketCount == 0U);

    unsigned got = 0;

    for (unsigned pass = 0; pass < 80 && got < 8U; pass++)
    {
        uint8_t in[300];
        int n = HostRx(in, sizeof(in));

        if (n > 0)
        {
            CHECK(n == 61);
            CHECK(in[0] == 0x04);
            CHECK(in[1] == 0xFF);
            CHECK(in[2] == 58);
            CHECK(in[3] == (uint8_t)(0x10 + got));
            CHECK(in[60] == (uint8_t)(0x10 + got));
            got++;
        }
        Run(4);
    }

    CHECK(got == 8U);
}

int main(void)
{
    TestCommandEvent();
    TestBadSlot();
    TestAcl();
    TestFullRing();

    printf("\n%d passed, %d failed\n", g_Pass, g_Fail);
    return g_Fail == 0 ? 0 : 1;
}

/**-------------------------------------------------------------------------
@file	hci_host_ipc.cpp

@brief	IPC host transport of HciController, the host on another core.

		The host is reached through BtHciIpc (IOsonata). It is a packet
		interface like native USB: one ring slot is one HCI packet and
		DevAddr is the packet type, so nothing is framed from a byte stream.
		HciController is the service provider, so it is the IPC owner: it
		partitions and formats the shared memory block and the host core
		attaches to what it finds there. board.h gives the block
		(HCI_IPC_MEM_ADDR, HCI_IPC_MEM_SIZE) and the channels.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "board.h"
#include "hci_app.h"
#include <string.h>
#include "hci_trace.h"

#include "bluetooth/bt_hci_ipc.h"

#ifndef HCI_IPC_MEM_ADDR
#error "board.h must define HCI_IPC_MEM_ADDR, the shared memory block address"
#endif
#ifndef HCI_IPC_MEM_SIZE
#error "board.h must define HCI_IPC_MEM_SIZE, the shared memory block size"
#endif
#ifndef HCI_IPC_NB_PKT
#define HCI_IPC_NB_PKT            8U
#endif
#ifndef HCI_IPC_TX_CHAN
#define HCI_IPC_TX_CHAN           0
#endif
#ifndef HCI_IPC_RX_CHAN
#define HCI_IPC_RX_CHAN           1
#endif
#ifndef HCI_IPC_IRQ_PRIORITY
#define HCI_IPC_IRQ_PRIORITY      6
#endif

static_assert(BT_HCI_IPC_MEMSIZE(HCI_IPC_NB_PKT) <= HCI_IPC_MEM_SIZE,
              "the IPC rings do not fit the shared memory block");
static_assert(BT_HCI_IPC_PKT_MAXLEN >= HCI_MSG_BUFFER_MAX_SIZE &&
              BT_HCI_IPC_PKT_MAXLEN >= HCI_SDC_ISO_PACKET_SIZE,
              "an IPC slot must hold the largest packet the controller sends");

static BtHciIpcIntrf s_HciIpc;

/* RX data and TX room are the two reasons to run the host side again. */
static int HciHostIpcEvent(DevIntrf_t *, DEVINTRF_EVT, uint8_t *, int Len)
{
    HciAppWake();
    return Len;
}

bool HciHostIpcInit(HciApp_t *pApp)
{
    BtHciIpcIntrfCfg_t cfg = {};

    cfg.pMemBlk = reinterpret_cast<uint8_t *>(HCI_IPC_MEM_ADDR);
    cfg.MemSize = HCI_IPC_MEM_SIZE;
    cfg.bOwner = true;
    cfg.NbPkt = HCI_IPC_NB_PKT;
    cfg.TxChan = HCI_IPC_TX_CHAN;
    cfg.RxChan = HCI_IPC_RX_CHAN;
    cfg.IntPrio = HCI_IPC_IRQ_PRIORITY;
    cfg.EvtCB = HciHostIpcEvent;

    if (!s_HciIpc.Init(cfg))
    {
        HciTrace("ipc: owner init failed addr=0x%08lX size=%lu pkt=%u\r\n",
                 (unsigned long)HCI_IPC_MEM_ADDR,
                 (unsigned long)HCI_IPC_MEM_SIZE, (unsigned)HCI_IPC_NB_PKT);
        return false;
    }

    /* The first words of the block show whether it was formatted. */
    const volatile IpcIntrfHdr_t *pHdr =
        reinterpret_cast<const volatile IpcIntrfHdr_t *>(HCI_IPC_MEM_ADDR);
    HciTrace("ipc: owner addr=0x%08lX size=%lu pkt=%u tx=%d rx=%d magic=0x%08lX ver=%lu\r\n",
             (unsigned long)HCI_IPC_MEM_ADDR, (unsigned long)HCI_IPC_MEM_SIZE,
             (unsigned)HCI_IPC_NB_PKT, HCI_IPC_TX_CHAN, HCI_IPC_RX_CHAN,
             (unsigned long)pHdr->Magic, (unsigned long)pHdr->Version);

    pApp->pHostIntrf = s_HciIpc.Data();
    return true;
}

/* Link state of the shared block, traced when it changes: the app core
 * attaching moves the ring indexes and the wait words. */
void HciHostIpcMonitor(void)
{
    static uint32_t s_Last[6];
    static bool s_First = true;
    const volatile IpcIntrfHdr_t *pHdr =
        reinterpret_cast<const volatile IpcIntrfHdr_t *>(HCI_IPC_MEM_ADDR);

    if (pHdr->Magic != IPC_INTRF_MAGIC)
    {
        return;
    }

    const volatile CFifo_t *pOut = reinterpret_cast<const volatile CFifo_t *>(
        HCI_IPC_MEM_ADDR + pHdr->Ring[0].Off);
    const volatile CFifo_t *pIn = reinterpret_cast<const volatile CFifo_t *>(
        HCI_IPC_MEM_ADDR + pHdr->Ring[1].Off);
    uint32_t now[6] = { pHdr->Epoch, pOut->PutIdx, pOut->GetIdx,
                        pIn->PutIdx, pIn->GetIdx,
                        pHdr->Wait[0] | (pHdr->Wait[1] << 1) };

    if (!s_First && memcmp(now, s_Last, sizeof(now)) == 0)
    {
        return;
    }
    s_First = false;
    memcpy(s_Last, now, sizeof(now));
    HciTrace("ipc: epoch=%lu out put=%lu get=%lu in put=%lu get=%lu wait=%lu\r\n",
             (unsigned long)now[0], (unsigned long)now[1], (unsigned long)now[2],
             (unsigned long)now[3], (unsigned long)now[4], (unsigned long)now[5]);
}

void HciHostIpcRelease(HciApp_t *pApp)
{
    if (pApp->pHostIntrf == s_HciIpc.Data())
    {
        s_HciIpc.Disable();
        pApp->pHostIntrf = nullptr;
    }
}

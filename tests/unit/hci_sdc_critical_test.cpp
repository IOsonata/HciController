/*
 * Regression tests for the SDC routing fixes that sit below the command table.
 * These use only the generic HciSdc layer, so they run without sdk-nrfxlib.
 */

#include "hci_sdc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct FakeSdc
{
    int32_t GetResult;
    uint8_t GetType;
    uint8_t GetPacket[64];
    unsigned AclCount;
    unsigned GetCount;
};

static int32_t AclPut(void *pContext, const uint8_t *)
{
    FakeSdc *pSdc = static_cast<FakeSdc *>(pContext);
    pSdc->AclCount++;
    return 0;
}

static int32_t Get(void *pContext, uint8_t *pPacket, uint8_t *pType)
{
    FakeSdc *pSdc = static_cast<FakeSdc *>(pContext);
    pSdc->GetCount++;

    if (pSdc->GetResult == 0)
    {
        memcpy(pPacket, pSdc->GetPacket, sizeof(pSdc->GetPacket));
        *pType = pSdc->GetType;
    }

    return pSdc->GetResult;
}

static HciCmdResult_t Reset(void *, const uint8_t *, size_t, uint8_t *, size_t)
{
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 0U};
}

static const HciCmdEntry_t s_Commands[] = {
    {0x0C03U, 0U, 0U, HCI_CMD_RESPONSE_COMPLETE, Reset},
};

static void Init(HciSdc_t *pSdc, FakeSdc *pBackend, uint8_t *pCommandEvent,
                 size_t CommandEventCapacity)
{
    HciSdcOps_t ops = {};
    ops.AclPut = AclPut;
    ops.Get = Get;
    ops.pContext = pBackend;
    ops.RetryError = HCI_SDC_RETRY_ERROR;

    assert(HciSdcInit(pSdc, &ops, s_Commands,
                      sizeof(s_Commands) / sizeof(s_Commands[0]), NULL,
                      pCommandEvent, CommandEventCapacity));
}

static void MakeAcl(uint16_t Handle, uint8_t Packet[HCI_SDC_ACL_HEADER_SIZE])
{
    Packet[0] = (uint8_t)Handle;
    Packet[1] = (uint8_t)((Handle >> 8) & 0x0FU);
    Packet[2] = 0U;
    Packet[3] = 0U;
}

static void TestEveryTrackedLinkCanOweCredit(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    assert(controller != NULL);

    /* One accepted packet consumes the controller's single advertised slot. */
    HciSdcSetAclLimit(&sdc, 1U);
    uint8_t acl[HCI_SDC_ACL_HEADER_SIZE];
    MakeAcl(0U, acl);
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));
    assert(backend.AclCount == 1U);
    assert(sdc.AclOutstandingTotal == 1U);

    /*
     * Every configured/tracked link can independently be refused while the
     * pool is full. Each refusal spent a host buffer, so each must fit in the
     * credit event table rather than overflowing after the fourth handle.
     */
    for (uint16_t handle = 0U; handle < HCI_SDC_ACL_TRACK_HANDLES; handle++)
    {
        MakeAcl(handle, acl);
        assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                               acl, sizeof(acl)));
    }

    assert(sdc.CreditEntries == HCI_SDC_ACL_TRACK_HANDLES);
    assert(sdc.CreditOverflowCount == 0U);

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
    assert(packet[2] == HCI_SDC_ACL_TRACK_HANDLES);
    assert(packetLen == 3U + ((size_t)HCI_SDC_ACL_TRACK_HANDLES * 4U));
    assert(sdc.CreditEntries == 0U);

    printf("[ok] all %u tracked links fit in one ACL credit event\n",
           (unsigned)HCI_SDC_ACL_TRACK_HANDLES);
}

static void TestSyntheticCreditsCannotStarveControllerQueue(void)
{
    HciSdc_t sdc;
    FakeSdc backend = {};
    uint8_t commandEvent[80];
    Init(&sdc, &backend, commandEvent, sizeof(commandEvent));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    assert(controller != NULL);
    HciSdcSetAclLimit(&sdc, 1U);

    uint8_t acl[HCI_SDC_ACL_HEADER_SIZE];
    MakeAcl(0U, acl);
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));

    /* First over-limit packet creates a synthetic credit event. */
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));
    assert(sdc.CreditEntries == 1U);

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    uint8_t packet[80];
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
    assert(backend.GetCount == 0U);

    /*
     * The host spends the returned credit immediately and creates another
     * synthetic event before the next Get. A real controller event is already
     * waiting. The next Get must poll SDC rather than emit the new credit.
     */
    MakeAcl(1U, acl);
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL,
                           acl, sizeof(acl)));
    assert(sdc.CreditEntries == 1U);

    backend.GetResult = 0;
    backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
    backend.GetPacket[0] = 0x3EU;
    backend.GetPacket[1] = 1U;
    backend.GetPacket[2] = 0xAAU;

    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(backend.GetCount == 1U);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(packet[0] == 0x3EU && packet[2] == 0xAAU);
    assert(sdc.CreditEntries == 1U);

    /* Once SDC has had its turn, the deferred synthetic credit may go out. */
    backend.GetResult = HCI_SDC_RETRY_ERROR;
    assert(controller->Get(controller->pContext, &type, packet,
                           sizeof(packet), &packetLen) ==
           HCI_CONTROLLER_GET_PACKET);
    assert(packet[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
    assert(backend.GetCount == 1U);

    printf("[ok] synthetic ACL credits cannot starve the controller queue\n");
}

int main(void)
{
    TestEveryTrackedLinkCanOweCredit();
    TestSyntheticCreditsCannotStarveControllerQueue();
    printf("All critical SDC routing tests passed.\n");
    return 0;
}

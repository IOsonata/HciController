#include "hci_sdc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct FakeSdc
{
    int32_t AclResult;
    int32_t IsoResult;
    int32_t GetResult;
    uint8_t GetType;
    uint8_t GetPacket[32];
    unsigned AclCount;
    unsigned IsoCount;
    unsigned GetCount;
    unsigned ProcessCount;
};

static int32_t AclPut(void *pContext, const uint8_t *)
{
    FakeSdc *sdc = static_cast<FakeSdc *>(pContext);
    sdc->AclCount++;
    return sdc->AclResult;
}

static int32_t IsoPut(void *pContext, const uint8_t *)
{
    FakeSdc *sdc = static_cast<FakeSdc *>(pContext);
    sdc->IsoCount++;
    return sdc->IsoResult;
}

static int32_t Get(void *pContext, uint8_t *pPacket, uint8_t *pType)
{
    FakeSdc *sdc = static_cast<FakeSdc *>(pContext);
    sdc->GetCount++;
    if (sdc->GetResult == 0)
    {
        memcpy(pPacket, sdc->GetPacket, sizeof(sdc->GetPacket));
        *pType = sdc->GetType;
    }
    return sdc->GetResult;
}

static void Process(void *pContext)
{
    static_cast<FakeSdc *>(pContext)->ProcessCount++;
}

static HciCmdResult_t Reset(void *, const uint8_t *, size_t, uint8_t *, size_t)
{
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 0U};
}

int main()
{
    static const HciCmdEntry_t commands[] = {
        {0x0C03U, 0U, 0U, HCI_CMD_RESPONSE_COMPLETE, Reset},
    };

    FakeSdc fake = {};
    fake.GetResult = HCI_SDC_RETRY_ERROR;

    HciSdcOps_t ops = {
        AclPut,
        IsoPut,
        Get,
        Process,
        &fake,
        HCI_SDC_RETRY_ERROR,
    };

    uint8_t commandEvent[80];
    HciSdc_t sdc;
    assert(HciSdcInit(&sdc,
                      &ops,
                      commands,
                      sizeof(commands) / sizeof(commands[0]),
                      NULL,
                      commandEvent,
                      sizeof(commandEvent)));

    const HciControllerOps_t *controller = HciSdcGetControllerOps(&sdc);
    assert(controller != NULL);

    const uint8_t reset[] = {0x03U, 0x0CU, 0x00U};
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_COMMAND, reset, sizeof(reset)));

    uint8_t packet[32];
    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t packetLen = 0U;
    assert(controller->Get(controller->pContext, &type, packet, sizeof(packet), &packetLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(packetLen == 6U);
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[3] == 0x03U && packet[4] == 0x0CU && packet[5] == HCI_STATUS_SUCCESS);

    const uint8_t acl[] = {0x01U, 0x20U, 0x00U, 0x00U};
    fake.AclResult = HCI_SDC_RETRY_ERROR;
    assert(!controller->Put(controller->pContext, HCI_H4_PACKET_ACL, acl, sizeof(acl)));
    assert(sdc.PutRetryCount == 1U);
    fake.AclResult = 0;
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ACL, acl, sizeof(acl)));
    assert(fake.AclCount == 2U);

    const uint8_t iso[] = {0x01U, 0x00U, 0x00U, 0x00U};
    assert(controller->Put(controller->pContext, HCI_H4_PACKET_ISO, iso, sizeof(iso)));
    assert(fake.IsoCount == 1U);

    fake.GetResult = 0;
    fake.GetType = HCI_SDC_MSG_TYPE_EVENT;
    fake.GetPacket[0] = 0x0EU;
    fake.GetPacket[1] = 0x04U;
    assert(controller->Get(controller->pContext, &type, packet, sizeof(packet), &packetLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT && packetLen == 6U);

    fake.GetType = HCI_SDC_MSG_TYPE_ACL;
    fake.GetPacket[0] = 0x01U;
    fake.GetPacket[1] = 0x20U;
    fake.GetPacket[2] = 0x03U;
    fake.GetPacket[3] = 0x00U;
    assert(controller->Get(controller->pContext, &type, packet, sizeof(packet), &packetLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_ACL && packetLen == 7U);

    fake.GetType = HCI_SDC_MSG_TYPE_ISO;
    fake.GetPacket[0] = 0x01U;
    fake.GetPacket[1] = 0x00U;
    fake.GetPacket[2] = 0x03U;
    fake.GetPacket[3] = 0xC0U;
    assert(controller->Get(controller->pContext, &type, packet, sizeof(packet), &packetLen) == HCI_CONTROLLER_GET_PACKET);
    assert(type == HCI_H4_PACKET_ISO && packetLen == 7U);

    fake.GetResult = HCI_SDC_RETRY_ERROR;
    assert(controller->Get(controller->pContext, &type, packet, sizeof(packet), &packetLen) == HCI_CONTROLLER_GET_EMPTY);

    controller->Process(controller->pContext);
    assert(fake.ProcessCount == 1U);

    /*
     * A steady command stream must not starve the controller queue. Both
     * sources share one outgoing slot per Get, and a host is entitled to keep
     * a command in flight because every Command Complete returns a credit.
     * Taking the command event whenever one exists means sdc_hci_get is never
     * reached and ACL data never leaves the controller.
     */
    {
        HciSdc_t busy;
        FakeSdc backend = {};
        uint8_t busyEvent[80];

        /* One ACL packet permanently waiting. */
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_ACL;
        backend.GetPacket[0] = 0x01U;
        backend.GetPacket[1] = 0x20U;
        backend.GetPacket[2] = 0x02U;
        backend.GetPacket[3] = 0x00U;

        HciSdcOps_t busyOps = {
            AclPut, IsoPut, Get, Process, &backend, HCI_SDC_RETRY_ERROR,
        };

        assert(HciSdcInit(&busy, &busyOps, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          busyEvent, sizeof(busyEvent)));

        const HciControllerOps_t *ops2 = HciSdcGetControllerOps(&busy);
        unsigned events = 0U;
        unsigned aclOut = 0U;

        for (unsigned pass = 0U; pass < 200U; pass++)
        {
            /* The host has a command ready every single pass. */
            (void)ops2->Put(ops2->pContext, HCI_H4_PACKET_COMMAND, reset,
                            sizeof(reset));

            HciH4PacketType_t outType = HCI_H4_PACKET_NONE;
            uint8_t out[64];
            size_t outLen = 0U;
            if (ops2->Get(ops2->pContext, &outType, out, sizeof(out),
                          &outLen) != HCI_CONTROLLER_GET_PACKET)
            {
                continue;
            }

            if (outType == HCI_H4_PACKET_ACL)
            {
                aclOut++;
            }
            else if (outType == HCI_H4_PACKET_EVENT)
            {
                events++;
            }
        }

        assert(backend.GetCount > 0U);
        assert(aclOut > 0U);
        assert(events > 0U);

        /* Neither source may take more than about half the slots. */
        assert(aclOut >= 90U && aclOut <= 110U);
        assert(events >= 90U && events <= 110U);

        printf("[ok] command stream and controller queue share the slot, "
               "%u events %u acl\n", events, aclOut);
    }

    printf("All SDC routing tests passed.\n");
    return 0;
}

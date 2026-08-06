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

    /*
     * A command handler runs while the command is accepted, so anything it
     * queues in the controller was queued after the response was built. The
     * response must still reach the host first, Vol 4 Part E 4.4 and 7.8.13.
     * Sharing the outgoing slot must not reorder that.
     */
    {
        HciSdc_t ordered;
        FakeSdc backend = {};
        uint8_t orderedEvent[80];

        HciSdcOps_t orderedOps = {
            AclPut, IsoPut, Get, Process, &backend, HCI_SDC_RETRY_ERROR,
        };
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        assert(HciSdcInit(&ordered, &orderedOps, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          orderedEvent, sizeof(orderedEvent)));

        const HciControllerOps_t *ops3 = HciSdcGetControllerOps(&ordered);
        uint8_t seen[8];
        unsigned seenCount = 0U;
        bool accepted[2] = {false, false};

        for (unsigned pass = 0U; pass < 8U; pass++)
        {
            /* Two commands back to back, the second queues a controller event. */
            if (!accepted[0])
            {
                accepted[0] = ops3->Put(ops3->pContext, HCI_H4_PACKET_COMMAND,
                                        reset, sizeof(reset));
            }
            else if (!accepted[1])
            {
                accepted[1] = ops3->Put(ops3->pContext, HCI_H4_PACKET_COMMAND,
                                        reset, sizeof(reset));
                if (accepted[1])
                {
                    /* The controller now has an event waiting. */
                    backend.GetResult = 0;
                    backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
                    backend.GetPacket[0] = 0x3EU;
                    backend.GetPacket[1] = 0x01U;
                    backend.GetPacket[2] = 0x01U;
                }
            }

            HciH4PacketType_t outType = HCI_H4_PACKET_NONE;
            uint8_t out[64];
            size_t outLen = 0U;
            if (ops3->Get(ops3->pContext, &outType, out, sizeof(out),
                          &outLen) == HCI_CONTROLLER_GET_PACKET)
            {
                assert(seenCount < sizeof(seen));
                seen[seenCount++] = out[0];
                if (out[0] == 0x3EU)
                {
                    backend.GetResult = HCI_SDC_RETRY_ERROR;
                }
            }
        }

        /* Both Command Completes must precede the controller event. */
        assert(seenCount >= 3U);
        assert(seen[0] == HCI_EVENT_COMMAND_COMPLETE);
        assert(seen[1] == HCI_EVENT_COMMAND_COMPLETE);
        assert(seen[2] == 0x3EU);
        printf("[ok] a queued controller event cannot overtake a command "
               "response\n");
    }

    /*
     * The host spends a buffer when it sends an ACL packet and only gets it
     * back in a Number Of Completed Packets event. A packet the controller
     * refuses must still return the credit, or the host runs out of buffers
     * on that link and never recovers. Vol 4 Part E 4.1.1 and 7.7.19.
     */
    {
        HciSdc_t credit;
        FakeSdc backend = {};
        uint8_t creditEvent[80];

        HciSdcOps_t creditOps = {
            AclPut, IsoPut, Get, Process, &backend, HCI_SDC_RETRY_ERROR,
        };
        backend.GetResult = HCI_SDC_RETRY_ERROR;
        backend.AclResult = -1;          /* the controller refuses every one */

        assert(HciSdcInit(&credit, &creditOps, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          creditEvent, sizeof(creditEvent)));

        const HciControllerOps_t *ops4 = HciSdcGetControllerOps(&credit);

        /* Three packets on handle 0x0001, two on handle 0x0002. */
        const uint8_t aclOne[] = {0x01U, 0x00U, 0x01U, 0x00U, 0xAAU};
        const uint8_t aclTwo[] = {0x02U, 0x20U, 0x01U, 0x00U, 0xBBU};
        for (unsigned i = 0U; i < 3U; i++)
        {
            assert(ops4->Put(ops4->pContext, HCI_H4_PACKET_ACL, aclOne,
                             sizeof(aclOne)));
        }
        for (unsigned i = 0U; i < 2U; i++)
        {
            assert(ops4->Put(ops4->pContext, HCI_H4_PACKET_ACL, aclTwo,
                             sizeof(aclTwo)));
        }
        assert(credit.AclPutErrorCount == 5U);

        HciH4PacketType_t creditType = HCI_H4_PACKET_NONE;
        uint8_t out[64];
        size_t outLen = 0U;
        assert(ops4->Get(ops4->pContext, &creditType, out, sizeof(out),
                         &outLen) == HCI_CONTROLLER_GET_PACKET);

        assert(creditType == HCI_H4_PACKET_EVENT);
        assert(out[0] == 0x13U);          /* Number Of Completed Packets */
        assert(out[1] == 9U);             /* 1 + 2 handles * 4 */
        assert(outLen == 11U);
        assert(out[2] == 2U);             /* Num_Handles */
        assert(out[3] == 0x01U && out[4] == 0x00U);   /* handle 0x0001 */
        assert(out[5] == 3U && out[6] == 0U);         /* three packets */
        /* The upper nibble of the ACL header is flags, not the handle. */
        assert(out[7] == 0x02U && out[8] == 0x00U);   /* handle 0x0002 */
        assert(out[9] == 2U && out[10] == 0U);        /* two packets */

        /* Emptied by the one event, so it does not repeat. */
        assert(ops4->Get(ops4->pContext, &creditType, out, sizeof(out),
                         &outLen) == HCI_CONTROLLER_GET_EMPTY);

        printf("[ok] a refused ACL packet returns its flow control credit\n");

        /*
         * A packet larger than the controller advertises never reaches the
         * controller, and still returns its credit. The host is not entitled
         * to send it, Vol 4 Part E 7.8.2.
         */
        backend.AclResult = 0;
        backend.AclCount = 0U;

        static uint8_t oversize[HCI_SDC_ACL_MAX_PAYLOAD + 8U];
        memset(oversize, 0, sizeof(oversize));
        oversize[0] = 0x03U;                     /* handle 0x0003 */
        assert(ops4->Put(ops4->pContext, HCI_H4_PACKET_ACL, oversize,
                         sizeof(oversize)));
        assert(credit.AclOversizeCount == 1U);
        assert(backend.AclCount == 0U);          /* never handed over */

        /* The largest packet that is allowed still goes through. */
        assert(ops4->Put(ops4->pContext, HCI_H4_PACKET_ACL, oversize,
                         HCI_SDC_ACL_HEADER_SIZE + HCI_SDC_ACL_MAX_PAYLOAD));
        assert(backend.AclCount == 1U);

        assert(ops4->Get(ops4->pContext, &creditType, out, sizeof(out),
                         &outLen) == HCI_CONTROLLER_GET_PACKET);
        assert(out[0] == 0x13U);
        assert(out[2] == 1U);
        assert(out[3] == 0x03U && out[4] == 0x00U);
        assert(out[5] == 1U);
        printf("[ok] an oversize ACL packet is refused and still returns "
               "its credit\n");
    }

    /*
     * The host is held to the buffer count the controller advertised. SDC
     * answers 0 for a packet past that count and then discards it, taking the
     * host's buffer with it, so the packet is refused here and the credit
     * handed back instead.
     *
     * The two properties that matter are opposite ones: everything inside the
     * limit goes through untouched, and only what is provably over is refused.
     * A guard that refuses traffic a host is entitled to send would be worse
     * than the loss it exists to prevent.
     */
    {
        HciSdc_t limit;
        FakeSdc backend = {};
        uint8_t limitEvent[80];

        HciSdcOps_t limitOps = {
            AclPut, IsoPut, Get, Process, &backend, HCI_SDC_RETRY_ERROR,
        };
        backend.GetResult = HCI_SDC_RETRY_ERROR;
        backend.AclResult = 0;

        assert(HciSdcInit(&limit, &limitOps, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          limitEvent, sizeof(limitEvent)));

        const HciControllerOps_t *ops5 = HciSdcGetControllerOps(&limit);
        const uint8_t aclFive[] = {0x05U, 0x00U, 0x01U, 0x00U, 0xCCU};

        /* Nothing is enforced until the host has been told a number. */
        for (unsigned i = 0U; i < 8U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 8U);
        assert(limit.AclCreditOverrunCount == 0U);
        printf("[ok] no limit is enforced before the host is told one\n");

#if HCI_SDC_ENFORCE_ACL_CREDITS
        /*
         * Now it has. Four more fit, the fifth does not.
         *
         * The reset clears what the unenforced phase above left in flight.
         * Reaching into AclOutstanding to do it would leave the running total
         * saying otherwise, and the guard reads the total.
         */
        HciSdcSetAclLimit(&limit, 4U);
        HciSdcResetFlowControl(&limit);

        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 12U);
        assert(limit.AclCreditOverrunCount == 0U);

        assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                         sizeof(aclFive)));
        assert(limit.AclPutCount == 12U);
        assert(limit.AclCreditOverrunCount == 1U);
        printf("[ok] the packet past the advertised count is refused\n");

        /* Refused, so the credit comes back rather than being lost. */
        HciH4PacketType_t limitType = HCI_H4_PACKET_NONE;
        uint8_t limitOut[64];
        size_t limitLen = 0U;
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        assert(limitType == HCI_H4_PACKET_EVENT);
        assert(limitOut[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
        assert(limitOut[2] == 1U);
        assert(limitOut[3] == 0x05U && limitOut[4] == 0x00U);
        assert(limitOut[5] == 1U && limitOut[6] == 0U);
        printf("[ok] the refused packet gets its credit back\n");

        /*
         * A completion frees the link again, which is what stops the guard
         * from being a one way ratchet that wedges a busy connection.
         */
        const uint8_t completed[] = {HCI_SDC_EVENT_NUM_COMPLETED_PACKETS, 5U,
                                     1U, 0x05U, 0x00U, 0x04U, 0x00U};
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(backend.GetPacket, completed, sizeof(completed));
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 16U);
        assert(limit.AclCreditOverrunCount == 1U);
        printf("[ok] a completion lets the link send again\n");

        /* A disconnection discards what the controller held, so it resets. */
        const uint8_t gone[] = {HCI_SDC_EVENT_DISCONNECTION_COMPLETE, 4U,
                                0x00U, 0x05U, 0x00U, 0x13U};
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(backend.GetPacket, gone, sizeof(gone));
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 20U);
        assert(limit.AclCreditOverrunCount == 1U);
        printf("[ok] a disconnection clears what the link had in flight\n");

        /*
         * Fill the link again so there is something to lose, then report a
         * Disconnection Complete with a non zero status. Vol 4 Part E 7.7.5
         * makes that a disconnection which did not happen, so the link is
         * still up and what is in flight on it is still in flight. Clearing
         * the count there would hand the host a whole fresh allowance on top
         * of packets the controller has not returned yet.
         */
        for (unsigned i = 0U; i < 4U; i++)
        {
            (void)ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                            sizeof(aclFive));
        }

        const uint8_t failed[] = {HCI_SDC_EVENT_DISCONNECTION_COMPLETE, 4U,
                                  0x0CU, 0x05U, 0x00U, 0x13U};
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(backend.GetPacket, failed, sizeof(failed));
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        const uint32_t putAfterFailed = limit.AclPutCount;
        const uint32_t overrunAfterFailed = limit.AclCreditOverrunCount;
        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == putAfterFailed);
        assert(limit.AclCreditOverrunCount == overrunAfterFailed + 4U);
        printf("[ok] a failed disconnection leaves the count alone\n");

        /*
         * HCI_Reset drops every connection and reports none of them, Vol 4
         * Part E 7.3.2, so nothing else clears this and a handle handed out
         * again after a reset would inherit the stale count and stay stalled.
         */
        HciSdcResetFlowControl(&limit);
        const uint32_t putAfterReset = limit.AclPutCount;
        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == putAfterReset + 4U);
        printf("[ok] a reset gives the link its allowance back\n");

        /*
         * The budget is one pool spread across every link, not an allowance
         * each. Vol 4 Part E 4.1.1 gives the host one buffer count and LE Read
         * Buffer Size reports that one number, so two links sharing a limit of
         * four get four packets between them and not four each.
         *
         * This is what the guard used to get wrong. Testing each link against
         * the whole number let N links hold N times what the controller owns,
         * and it was invisible while the build allowed only one connection.
         */
        HciSdcResetFlowControl(&limit);
        const uint8_t aclSix[] = {0x06U, 0x00U, 0x01U, 0x00U, 0xDDU};
        const uint32_t putBeforeShare = limit.AclPutCount;
        const uint32_t overrunBeforeShare = limit.AclCreditOverrunCount;

        for (unsigned i = 0U; i < 2U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclSix,
                             sizeof(aclSix)));
        }
        assert(limit.AclPutCount == putBeforeShare + 4U);
        assert(limit.AclCreditOverrunCount == overrunBeforeShare);

        /* The pool is spent. Neither link may send, however little it sent. */
        assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclFive,
                         sizeof(aclFive)));
        assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclSix,
                         sizeof(aclSix)));
        assert(limit.AclPutCount == putBeforeShare + 4U);
        assert(limit.AclCreditOverrunCount == overrunBeforeShare + 2U);
        printf("[ok] two links share one budget rather than getting one "
               "each\n");

        /*
         * Drain the credits the two refusals owe. They are built ahead of the
         * controller queue, so leaving them here would mean the next Get hands
         * back one of those instead of the event being injected below.
         */
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        assert(limitOut[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
        assert(limitOut[2] == 2U);

        /* What one link returns is spendable by the other. */
        const uint8_t freedOne[] = {HCI_SDC_EVENT_NUM_COMPLETED_PACKETS, 5U,
                                    1U, 0x05U, 0x00U, 0x02U, 0x00U};
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(backend.GetPacket, freedOne, sizeof(freedOne));
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        for (unsigned i = 0U; i < 2U; i++)
        {
            assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclSix,
                             sizeof(aclSix)));
        }
        assert(limit.AclPutCount == putBeforeShare + 6U);
        assert(limit.AclCreditOverrunCount == overrunBeforeShare + 2U);
        printf("[ok] a completion on one link frees the pool for the other\n");

        /*
         * A disconnection takes that link's share of the total with it and
         * nobody else's. Link six holds all four now, so dropping link five,
         * which holds none, must not hand anything back.
         */
        const uint8_t goneFive[] = {HCI_SDC_EVENT_DISCONNECTION_COMPLETE, 4U,
                                    0x00U, 0x05U, 0x00U, 0x13U};
        backend.GetResult = 0;
        backend.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(backend.GetPacket, goneFive, sizeof(goneFive));
        assert(ops5->Get(ops5->pContext, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_CONTROLLER_GET_PACKET);
        backend.GetResult = HCI_SDC_RETRY_ERROR;

        const uint32_t overrunBeforeGone = limit.AclCreditOverrunCount;
        assert(ops5->Put(ops5->pContext, HCI_H4_PACKET_ACL, aclSix,
                         sizeof(aclSix)));
        assert(limit.AclPutCount == putBeforeShare + 6U);
        assert(limit.AclCreditOverrunCount == overrunBeforeGone + 1U);
        printf("[ok] a disconnection returns only what that link held\n");
#else
        printf("[ok] credit enforcement is built out, nothing to check\n");
#endif
    }

    printf("All SDC routing tests passed.\n");
    return 0;
}

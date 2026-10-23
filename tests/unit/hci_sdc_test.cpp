#include "hci_sdc.h"
#include "sdc_hci.h"

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
};

/* The nrfxlib controller entry points hci_sdc.cpp calls, over a fake. */
static FakeSdc *s_pFake;

int32_t sdc_hci_data_put(uint8_t const *)
{
    s_pFake->AclCount++;
    return s_pFake->AclResult;
}

int32_t sdc_hci_iso_data_put(uint8_t const *)
{
    s_pFake->IsoCount++;
    return s_pFake->IsoResult;
}

int32_t sdc_hci_get(uint8_t *pPacket, uint8_t *pType)
{
    FakeSdc *sdc = s_pFake;
    sdc->GetCount++;
    if (sdc->GetResult == 0)
    {
        memcpy(pPacket, sdc->GetPacket, sizeof(sdc->GetPacket));
        *pType = sdc->GetType;
    }
    return sdc->GetResult;
}

static HciCmdResult_t Reset(void *, const uint8_t *, size_t, uint8_t *, size_t)
{
    return {HCI_STATUS_SUCCESS, HCI_CMD_RESPONSE_COMPLETE, 0U};
}

#if !HCI_SDC_LEGACY_PAWR_COMPLETION
static HciCmdResult_t PawrResponse(void *pContext, const uint8_t *, size_t,
                                  uint8_t *pReturn, size_t ReturnCapacity)
{
    assert(ReturnCapacity >= 2U);
    pReturn[0] = 0x34U;
    pReturn[1] = 0x02U;
    return {*static_cast<uint8_t *>(pContext), HCI_CMD_RESPONSE_COMPLETE, 2U};
}

/* Current SDC returns status directly and queues no second completion. */
static void TestImmediatePawrCompletion(void)
{
    const HciCmdEntry_t commands[] = {
        {0x0C03U, 0U, 0U, HCI_CMD_RESPONSE_COMPLETE, Reset},
        {0x2083U, 8U, 2U, HCI_CMD_RESPONSE_COMPLETE, PawrResponse},
    };
    const uint8_t statuses[] = {HCI_STATUS_SUCCESS, HCI_STATUS_COMMAND_DISALLOWED};
    for (uint8_t status : statuses)
    {
        FakeSdc fake = {};
        fake.GetResult = -NRF_EAGAIN;
        s_pFake = &fake;
        HciSdc_t sdc;
        uint8_t commandEvent[80];
        assert(HciSdcInit(&sdc, commands, 2U, &status,
                          commandEvent, sizeof(commandEvent)));
        HciSdc_t *controller = &sdc;
        const uint8_t response[] = {0x83U, 0x20U, 8U,
                                     0x34U, 0x02U, 0U, 0U, 1U, 1U, 0U, 0U};
        assert(HciSdcPutPacket(controller, HCI_H4_PACKET_COMMAND,
                                 response, sizeof(response)));
        uint8_t packet[32];
        HciH4PacketType_t type;
        size_t length;
        assert(HciSdcGetPacket(controller, &type, packet,
                                 sizeof(packet), &length) == HCI_SDC_GET_PACKET);
        assert(type == HCI_H4_PACKET_EVENT && length == 8U);
        const uint8_t expected[] = {0x0EU, 6U, 1U, 0x83U, 0x20U,
                                     status, 0x34U, 0x02U};
        assert(memcmp(packet, expected, sizeof(expected)) == 0);
        assert(!sdc.DelayedCommandPending);
        /* Drain the controller turn; no duplicate completion may appear. */
        assert(HciSdcGetPacket(controller, &type, packet,
                                 sizeof(packet), &length) == HCI_SDC_GET_EMPTY);
        const uint8_t reset[] = {0x03U, 0x0CU, 0U};
        assert(HciSdcPutPacket(controller, HCI_H4_PACKET_COMMAND,
                                 reset, sizeof(reset)));
        assert(HciSdcGetPacket(controller, &type, packet,
                                 sizeof(packet), &length) == HCI_SDC_GET_PACKET);
        assert(length == 6U && packet[3] == 0x03U && packet[4] == 0x0CU);
    }
    printf("[ok] PAwR completion preserves status, handle and subsequent Reset\n");
}
#endif

int main()
{
#if !HCI_SDC_LEGACY_PAWR_COMPLETION
    TestImmediatePawrCompletion();
#endif
    static const HciCmdEntry_t commands[] = {
        {0x0C03U, 0U, 0U, HCI_CMD_RESPONSE_COMPLETE, Reset},
    };

    FakeSdc fake = {};
    fake.GetResult = -NRF_EAGAIN;

    s_pFake = &fake;

    uint8_t commandEvent[80];
    HciSdc_t sdc;
    assert(HciSdcInit(&sdc,
                      commands,
                      sizeof(commands) / sizeof(commands[0]),
                      NULL,
                      commandEvent,
                      sizeof(commandEvent)));

    HciSdc_t *controller = &sdc;
    assert(controller != NULL);

    const uint8_t reset[] = {0x03U, 0x0CU, 0x00U};
    assert(HciSdcPutPacket(controller, HCI_H4_PACKET_COMMAND, reset, sizeof(reset)));

    uint8_t packet[32];
    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t packetLen = 0U;
    assert(HciSdcGetPacket(controller, &type, packet, sizeof(packet), &packetLen) == HCI_SDC_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT);
    assert(packetLen == 6U);
    assert(packet[0] == HCI_EVENT_COMMAND_COMPLETE);
    assert(packet[3] == 0x03U && packet[4] == 0x0CU && packet[5] == HCI_STATUS_SUCCESS);

    const uint8_t acl[] = {0x01U, 0x20U, 0x00U, 0x00U};
    fake.AclResult = -NRF_EAGAIN;
    assert(!HciSdcPutPacket(controller, HCI_H4_PACKET_ACL, acl, sizeof(acl)));
    assert(sdc.PutRetryCount == 1U);
    fake.AclResult = 0;
    assert(HciSdcPutPacket(controller, HCI_H4_PACKET_ACL, acl, sizeof(acl)));
    assert(fake.AclCount == 2U);

    const uint8_t iso[] = {0x01U, 0x00U, 0x00U, 0x00U};
    assert(HciSdcPutPacket(controller, HCI_H4_PACKET_ISO, iso, sizeof(iso)));
    assert(fake.IsoCount == 1U);

    fake.GetResult = 0;
    fake.GetType = HCI_SDC_MSG_TYPE_EVENT;
    fake.GetPacket[0] = 0x0EU;
    fake.GetPacket[1] = 0x04U;
    assert(HciSdcGetPacket(controller, &type, packet, sizeof(packet), &packetLen) == HCI_SDC_GET_PACKET);
    assert(type == HCI_H4_PACKET_EVENT && packetLen == 6U);

    fake.GetType = HCI_SDC_MSG_TYPE_ACL;
    fake.GetPacket[0] = 0x01U;
    fake.GetPacket[1] = 0x20U;
    fake.GetPacket[2] = 0x03U;
    fake.GetPacket[3] = 0x00U;
    assert(HciSdcGetPacket(controller, &type, packet, sizeof(packet), &packetLen) == HCI_SDC_GET_PACKET);
    assert(type == HCI_H4_PACKET_ACL && packetLen == 7U);

    fake.GetType = HCI_SDC_MSG_TYPE_ISO;
    fake.GetPacket[0] = 0x01U;
    fake.GetPacket[1] = 0x00U;
    fake.GetPacket[2] = 0x03U;
    fake.GetPacket[3] = 0xC0U;
    assert(HciSdcGetPacket(controller, &type, packet, sizeof(packet), &packetLen) == HCI_SDC_GET_PACKET);
    assert(type == HCI_H4_PACKET_ISO && packetLen == 7U);

    fake.GetResult = -NRF_EAGAIN;
    assert(HciSdcGetPacket(controller, &type, packet, sizeof(packet), &packetLen) == HCI_SDC_GET_EMPTY);

    /*
     * A steady command stream must not starve the controller queue. Both
     * sources share one outgoing slot per Get, and a host is entitled to keep
     * a command in flight because every Command Complete returns a credit.
     * Taking the command event whenever one exists means sdc_hci_get is never
     * reached and ACL data never leaves the controller.
     */
    {
        HciSdc_t busy;
        FakeSdc ctlr = {};
        uint8_t busyEvent[80];

        /* One ACL packet permanently waiting. */
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_ACL;
        ctlr.GetPacket[0] = 0x01U;
        ctlr.GetPacket[1] = 0x20U;
        ctlr.GetPacket[2] = 0x02U;
        ctlr.GetPacket[3] = 0x00U;

        s_pFake = &ctlr;

        assert(HciSdcInit(&busy, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          busyEvent, sizeof(busyEvent)));

        HciSdc_t *ops2 = &busy;
        unsigned events = 0U;
        unsigned aclOut = 0U;

        for (unsigned pass = 0U; pass < 200U; pass++)
        {
            /* The host has a command ready every single pass. */
            (void)HciSdcPutPacket(ops2, HCI_H4_PACKET_COMMAND, reset,
                            sizeof(reset));

            HciH4PacketType_t outType = HCI_H4_PACKET_NONE;
            uint8_t out[64];
            size_t outLen = 0U;
            if (HciSdcGetPacket(ops2, &outType, out, sizeof(out),
                          &outLen) != HCI_SDC_GET_PACKET)
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

        assert(ctlr.GetCount > 0U);
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
        FakeSdc ctlr = {};
        uint8_t orderedEvent[80];

        s_pFake = &ctlr;
        ctlr.GetResult = -NRF_EAGAIN;

        assert(HciSdcInit(&ordered, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          orderedEvent, sizeof(orderedEvent)));

        HciSdc_t *ops3 = &ordered;
        uint8_t seen[8];
        unsigned seenCount = 0U;
        bool accepted[2] = {false, false};

        for (unsigned pass = 0U; pass < 8U; pass++)
        {
            /* Two commands back to back, the second queues a controller event. */
            if (!accepted[0])
            {
                accepted[0] = HciSdcPutPacket(ops3, HCI_H4_PACKET_COMMAND,
                                        reset, sizeof(reset));
            }
            else if (!accepted[1])
            {
                accepted[1] = HciSdcPutPacket(ops3, HCI_H4_PACKET_COMMAND,
                                        reset, sizeof(reset));
                if (accepted[1])
                {
                    /* The controller now has an event waiting. */
                    ctlr.GetResult = 0;
                    ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
                    ctlr.GetPacket[0] = 0x3EU;
                    ctlr.GetPacket[1] = 0x01U;
                    ctlr.GetPacket[2] = 0x01U;
                }
            }

            HciH4PacketType_t outType = HCI_H4_PACKET_NONE;
            uint8_t out[64];
            size_t outLen = 0U;
            if (HciSdcGetPacket(ops3, &outType, out, sizeof(out),
                          &outLen) == HCI_SDC_GET_PACKET)
            {
                assert(seenCount < sizeof(seen));
                seen[seenCount++] = out[0];
                if (out[0] == 0x3EU)
                {
                    ctlr.GetResult = -NRF_EAGAIN;
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
        FakeSdc ctlr = {};
        uint8_t creditEvent[80];

        s_pFake = &ctlr;
        ctlr.GetResult = -NRF_EAGAIN;
        ctlr.AclResult = -1;          /* the controller refuses every one */

        assert(HciSdcInit(&credit, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          creditEvent, sizeof(creditEvent)));

        HciSdc_t *ops4 = &credit;

        /* Three packets on handle 0x0001, two on handle 0x0002. */
        const uint8_t aclOne[] = {0x01U, 0x00U, 0x01U, 0x00U, 0xAAU};
        const uint8_t aclTwo[] = {0x02U, 0x20U, 0x01U, 0x00U, 0xBBU};
        for (unsigned i = 0U; i < 3U; i++)
        {
            assert(HciSdcPutPacket(ops4, HCI_H4_PACKET_ACL, aclOne,
                             sizeof(aclOne)));
        }
        for (unsigned i = 0U; i < 2U; i++)
        {
            assert(HciSdcPutPacket(ops4, HCI_H4_PACKET_ACL, aclTwo,
                             sizeof(aclTwo)));
        }
        assert(credit.AclPutErrorCount == 5U);

        HciH4PacketType_t creditType = HCI_H4_PACKET_NONE;
        uint8_t out[64];
        size_t outLen = 0U;
        assert(HciSdcGetPacket(ops4, &creditType, out, sizeof(out),
                         &outLen) == HCI_SDC_GET_PACKET);

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
        assert(HciSdcGetPacket(ops4, &creditType, out, sizeof(out),
                         &outLen) == HCI_SDC_GET_EMPTY);

        printf("[ok] a refused ACL packet returns its flow control credit\n");

        /*
         * A packet larger than the controller advertises never reaches the
         * controller, and still returns its credit. The host is not entitled
         * to send it, Vol 4 Part E 7.8.2.
         */
        ctlr.AclResult = 0;
        ctlr.AclCount = 0U;

        static uint8_t oversize[HCI_SDC_ACL_MAX_PAYLOAD + 8U];
        memset(oversize, 0, sizeof(oversize));
        oversize[0] = 0x03U;                     /* handle 0x0003 */
        assert(HciSdcPutPacket(ops4, HCI_H4_PACKET_ACL, oversize,
                         sizeof(oversize)));
        assert(credit.AclOversizeCount == 1U);
        assert(ctlr.AclCount == 0U);          /* never handed over */

        /* The largest packet that is allowed still goes through. */
        assert(HciSdcPutPacket(ops4, HCI_H4_PACKET_ACL, oversize,
                         HCI_SDC_ACL_HEADER_SIZE + HCI_SDC_ACL_MAX_PAYLOAD));
        assert(ctlr.AclCount == 1U);

        assert(HciSdcGetPacket(ops4, &creditType, out, sizeof(out),
                         &outLen) == HCI_SDC_GET_PACKET);
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
        FakeSdc ctlr = {};
        uint8_t limitEvent[80];

        s_pFake = &ctlr;
        ctlr.GetResult = -NRF_EAGAIN;
        ctlr.AclResult = 0;

        assert(HciSdcInit(&limit, commands,
                          sizeof(commands) / sizeof(commands[0]), NULL,
                          limitEvent, sizeof(limitEvent)));

        HciSdc_t *ops5 = &limit;
        const uint8_t aclFive[] = {0x05U, 0x00U, 0x01U, 0x00U, 0xCCU};

        /* Nothing is enforced until the host has been told a number. */
        for (unsigned i = 0U; i < 8U; i++)
        {
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
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
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 12U);
        assert(limit.AclCreditOverrunCount == 0U);

        assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                         sizeof(aclFive)));
        assert(limit.AclPutCount == 12U);
        assert(limit.AclCreditOverrunCount == 1U);
        printf("[ok] the packet past the advertised count is refused\n");

        /* Refused, so the credit comes back rather than being lost. */
        HciH4PacketType_t limitType = HCI_H4_PACKET_NONE;
        uint8_t limitOut[64];
        size_t limitLen = 0U;
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
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
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(ctlr.GetPacket, completed, sizeof(completed));
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        ctlr.GetResult = -NRF_EAGAIN;

        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
        }
        assert(limit.AclPutCount == 16U);
        assert(limit.AclCreditOverrunCount == 1U);
        printf("[ok] a completion lets the link send again\n");

        /* A disconnection discards what the controller held, so it resets. */
        const uint8_t gone[] = {HCI_SDC_EVENT_DISCONNECTION_COMPLETE, 4U,
                                0x00U, 0x05U, 0x00U, 0x13U};
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(ctlr.GetPacket, gone, sizeof(gone));
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        ctlr.GetResult = -NRF_EAGAIN;

        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
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
            (void)HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                            sizeof(aclFive));
        }

        const uint8_t failed[] = {HCI_SDC_EVENT_DISCONNECTION_COMPLETE, 4U,
                                  0x0CU, 0x05U, 0x00U, 0x13U};
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(ctlr.GetPacket, failed, sizeof(failed));
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        ctlr.GetResult = -NRF_EAGAIN;

        const uint32_t putAfterFailed = limit.AclPutCount;
        const uint32_t overrunAfterFailed = limit.AclCreditOverrunCount;
        for (unsigned i = 0U; i < 4U; i++)
        {
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
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
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
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
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                             sizeof(aclFive)));
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclSix,
                             sizeof(aclSix)));
        }
        assert(limit.AclPutCount == putBeforeShare + 4U);
        assert(limit.AclCreditOverrunCount == overrunBeforeShare);

        /* The pool is spent. Neither link may send, however little it sent. */
        assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclFive,
                         sizeof(aclFive)));
        assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclSix,
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
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        assert(limitOut[0] == HCI_SDC_EVENT_NUM_COMPLETED_PACKETS);
        assert(limitOut[2] == 2U);

        /* What one link returns is spendable by the other. */
        const uint8_t freedOne[] = {HCI_SDC_EVENT_NUM_COMPLETED_PACKETS, 5U,
                                    1U, 0x05U, 0x00U, 0x02U, 0x00U};
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(ctlr.GetPacket, freedOne, sizeof(freedOne));
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        ctlr.GetResult = -NRF_EAGAIN;

        for (unsigned i = 0U; i < 2U; i++)
        {
            assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclSix,
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
        ctlr.GetResult = 0;
        ctlr.GetType = HCI_SDC_MSG_TYPE_EVENT;
        memcpy(ctlr.GetPacket, goneFive, sizeof(goneFive));
        assert(HciSdcGetPacket(ops5, &limitType, limitOut,
                         sizeof(limitOut), &limitLen) ==
               HCI_SDC_GET_PACKET);
        ctlr.GetResult = -NRF_EAGAIN;

        const uint32_t overrunBeforeGone = limit.AclCreditOverrunCount;
        assert(HciSdcPutPacket(ops5, HCI_H4_PACKET_ACL, aclSix,
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


/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_counters.h"

#include <string.h>

/*
 * Host-only tests deliberately link hci_counters.cpp without the SDC resource
 * configurator. Give those builds a zero-valued source. The firmware links the
 * strong definition in hci_sdc_resources.cpp, so a counter read sees the final
 * sdc_cfg_set result even though the counter object was wired before SDC start.
 */
extern "C" __attribute__((weak)) int32_t HciSdcResourcesRequired(void)
{
    return 0;
}

/*
 * Platform diagnostics are optional for the same reason. The nRF52840 native
 * USB DCD supplies the strong definition; portable/unit-test builds retain the
 * zeroes already present in the counter array.
 */
extern "C" __attribute__((weak))
void HciUsbPlatformReadCounters(uint32_t *pValues, size_t Count)
{
    (void)pValues;
    (void)Count;
}

static void HciCountersWriteLe32(uint8_t *pData, uint32_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
    pData[2] = (uint8_t)(Value >> 16);
    pData[3] = (uint8_t)(Value >> 24);
}

void HciCountersInit(HciCounters_t *pCounters,
                     HciSdc_t *pSdc,
                     const HciController_t *pController)
{
    if (pCounters == NULL)
    {
        return;
    }

    pCounters->pSdc = pSdc;
    pCounters->pController = pController;
    pCounters->SdcMemRequired = 0U;
    pCounters->SdcMemCapacity = 0U;
}

void HciCountersSetSdcMem(HciCounters_t *pCounters,
                          uint32_t Required,
                          uint32_t Capacity)
{
    if (pCounters == NULL)
    {
        return;
    }

    pCounters->SdcMemRequired = Required;
    pCounters->SdcMemCapacity = Capacity;
}

HciCmdResult_t HciCountersRead(void *pContext,
                               const uint8_t *,
                               size_t,
                               uint8_t *pReturn,
                               size_t ReturnCapacity)
{
    /*
     * Command Disallowed when the table carrying this row was given some other
     * command context, since the alternative is to read whatever that context
     * happens to point at and report it as counters.
     */
    HciCmdResult_t result = {HCI_STATUS_COMMAND_DISALLOWED,
                             HCI_CMD_RESPONSE_COMPLETE, 0U};

    const HciCounters_t *pCounters =
        static_cast<const HciCounters_t *>(pContext);
    if (pCounters == NULL || pReturn == NULL)
    {
        return result;
    }

    if (ReturnCapacity < HCI_COUNTERS_RETURN_LEN)
    {
        result.Status = HCI_STATUS_MEMORY_CAPACITY_EXCEEDED;
        return result;
    }

    uint32_t values[HCI_COUNTERS_COUNT];
    memset(values, 0, sizeof(values));

    const HciSdc_t *pSdc = pCounters->pSdc;
    if (pSdc != NULL)
    {
        values[0] = pSdc->Commands.CommandCount;
        values[1] = pSdc->Commands.UnknownCommandCount;
        values[2] = pSdc->Commands.InvalidPacketCount;
        values[3] = pSdc->Commands.InvalidParamLenCount;
        values[4] = pSdc->Commands.HandlerErrorCount;
        values[5] = pSdc->Commands.EventBackpressureCount;
        values[6] = pSdc->AclPutErrorCount;
        values[7] = pSdc->IsoPutErrorCount;
        values[8] = pSdc->PutRetryCount;
        values[9] = pSdc->GetErrorCount;
        values[10] = pSdc->InvalidOutputTypeCount;
        values[11] = pSdc->InvalidOutputLengthCount;
        values[12] = pSdc->CommandDeferredCount;
        values[13] = pSdc->AclOversizeCount;
        values[14] = pSdc->IsoDropCount;
        values[15] = pSdc->CreditOverflowCount;
        values[16] = pSdc->AclPutCount;
        values[17] = pSdc->IsoPutCount;
        values[30] = pSdc->AclCreditOverrunCount;
        values[31] = pSdc->AclTrackOverflowCount;
        values[34] = pSdc->PawrDelayedCandidateCount;
        values[35] = pSdc->PawrDelayedHandlerCallCount;
        values[36] = pSdc->PawrSyntheticSuppressedCount;
        values[37] = pSdc->PawrSdcCompleteCount;
    }

    const HciController_t *pController = pCounters->pController;
    if (pController != NULL)
    {
        values[18] = pController->Host.Parser.InvalidTypeCount;
        values[19] = pController->Host.Parser.OversizePacketCount;
        values[20] = pController->Host.Parser.DeliveryRetryCount;
        values[21] = pController->Host.RxErrorCount;
        values[22] = pController->Host.TxErrorCount;
        values[23] = pController->Host.TxBusyCount;
        values[24] = pController->Host.TxOversizeCount;
        values[25] = pController->HostPacketRetryCount;
        values[26] = pController->InvalidHostPacketCount;
        values[27] = pController->ControllerGetErrorCount;
        values[28] = pController->InvalidControllerPacketCount;
        values[29] = pController->UnsendableControllerPacketCount;
        values[38] = pController->ControllerAclPacketCount;
        values[39] = pController->HostAclPacketCount;
    }

    /*
     * The capacity is fixed when the application allocates the pool. The
     * required value is different: sdc_cfg_set produces it later, while the
     * target starts in the runtime thread. Prefer that live value when the
     * firmware has the resource module linked and retain the stored value as a
     * fallback for ports and host tests that do not.
     */
    const int32_t liveRequired = HciSdcResourcesRequired();
    values[32] = liveRequired > 0 ?
                 (uint32_t)liveRequired : pCounters->SdcMemRequired;
    values[33] = pCounters->SdcMemCapacity;

    HciUsbPlatformReadCounters(&values[HCI_COUNTERS_PLATFORM_FIRST],
                               HCI_COUNTERS_PLATFORM_COUNT);

    pReturn[0] = (uint8_t)HCI_COUNTERS_VERSION;
    for (size_t i = 0U; i < HCI_COUNTERS_COUNT; i++)
    {
        HciCountersWriteLe32(&pReturn[1U + (i * 4U)], values[i]);
    }

    result.Status = HCI_STATUS_SUCCESS;
    result.ReturnLen = HCI_COUNTERS_RETURN_LEN;
    return result;
}

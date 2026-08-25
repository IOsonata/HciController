/**-------------------------------------------------------------------------
@file	hci_controller.cpp

@brief	Host transport and HCI controller bridge implementation.

		Routes Host packets to the controller, sends controller packets to the
		selected Host transport, handles local vendor commands, startup H:4
		synchronization, and release Core-version filtering.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_controller.h"
#include "hci_core_profile.h"

#include <limits.h>
#include <string.h>

#define HCI_CONTROLLER_EVENT_COMMAND_COMPLETE          0x0EU
#define HCI_CONTROLLER_STATUS_SUCCESS                  0x00U
#define HCI_CONTROLLER_STATUS_UNKNOWN_HCI_COMMAND      0x01U
#define HCI_CONTROLLER_STATUS_MEMORY_CAPACITY_EXCEEDED 0x07U
#define HCI_CONTROLLER_STATUS_INVALID_HCI_PARAMETERS   0x12U
#define HCI_CONTROLLER_OPCODE_READ_LOCAL_VERSION       0x1001U
#define HCI_CONTROLLER_OPCODE_READ_SUPPORTED_COMMANDS  0x1002U
#define HCI_CONTROLLER_LOCAL_VERSION_EVENT_SIZE        14U
#define HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE      6U
#define HCI_CONTROLLER_COMMAND_HEADER_SIZE             3U
#define HCI_CONTROLLER_MAX_COMMAND_COMPLETE_RETURN_LEN 251U
#define HCI_CONTROLLER_LOOPBACK_EVENT_BASE_SIZE        \
    (HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE +       \
     HCI_CONTROLLER_LOOPBACK_RETURN_HEADER_LEN)
#define HCI_CONTROLLER_H4_STARTUP_RX_PASSES            64U

#define HCI_CONTROLLER_OPCODE_LE_READ_ALL_LOCAL_FEATURES  0x2087U
#define HCI_CONTROLLER_OPCODE_LE_READ_ALL_REMOTE_FEATURES 0x2088U
#define HCI_CONTROLLER_OPCODE_LE_SET_HOST_FEATURE_V2      0x2097U

static const uint8_t s_HciControllerH4Reset[] = {
    (uint8_t)HCI_H4_PACKET_COMMAND,
    0x03U,
    0x0CU,
    0x00U,
};

static uint16_t HciControllerReadLe16(const uint8_t *pData)
{
    return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

static void HciControllerWriteLe16(uint8_t *pData, uint16_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
}

static void HciControllerWriteLe32(uint8_t *pData, uint32_t Value)
{
    pData[0] = (uint8_t)Value;
    pData[1] = (uint8_t)(Value >> 8);
    pData[2] = (uint8_t)(Value >> 16);
    pData[3] = (uint8_t)(Value >> 24);
}

/* PRBS8: x^8 + x^6 + x^5 + x^4 + 1. The current state is the next octet. */
static uint8_t HciControllerPrbs8Next(uint8_t *pState)
{
    const uint8_t value = *pState;
    const uint8_t feedback = (uint8_t)(((value >> 7) ^ (value >> 5) ^
                                        (value >> 4) ^ (value >> 3)) & 1U);
    *pState = (uint8_t)((value << 1) | feedback);
    return value;
}

static uint32_t HciControllerCrc32(const uint8_t *pData, size_t Len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < Len; i++)
    {
        crc ^= pData[i];
        for (unsigned bit = 0U; bit < 8U; bit++)
        {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

/*
 * Optional transport-owned diagnostic. Native USB supplies the strong
 * definition. Serial-only and host-test builds therefore leave 0xFFF2
 * unsupported instead of growing a dependency from the generic bridge into a
 * particular host transport.
 */
extern "C" __attribute__((weak))
size_t HciUsbPlatformReadTxValidation(uint8_t *pData, size_t Capacity)
{
    (void)pData;
    (void)Capacity;
    return 0U;
}

bool HciControllerKnowsLocalCommand(uint16_t Opcode, size_t ParamLen)
{
    if (Opcode == HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE)
    {
        return ParamLen == 0U;
    }

    return Opcode == HCI_CONTROLLER_LOOPBACK_OPCODE &&
           ParamLen >= HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN &&
           ParamLen <= HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN +
                       HCI_CONTROLLER_LOOPBACK_MAX_DATA_LEN;
}

static bool HciControllerCommandAllowedByCore(uint16_t Opcode)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION != 0U && \
    HCI_CONTROLLER_TARGET_CORE_VERSION < HCI_CORE_VERSION_6_0
    switch (Opcode)
    {
        case HCI_CONTROLLER_OPCODE_LE_READ_ALL_LOCAL_FEATURES:
        case HCI_CONTROLLER_OPCODE_LE_READ_ALL_REMOTE_FEATURES:
        case HCI_CONTROLLER_OPCODE_LE_SET_HOST_FEATURE_V2:
            return false;

        default:
            break;
    }
#else
    (void)Opcode;
#endif
    return true;
}

static bool HciControllerQueueCommandStatus(HciController_t *pController,
                                            uint16_t Opcode,
                                            uint8_t Status)
{
    if (pController->ControllerPacketPending)
    {
        return false;
    }

    uint8_t *pEvent = pController->pControllerPacket;
    pEvent[0] = HCI_CONTROLLER_EVENT_COMMAND_COMPLETE;
    pEvent[1] = 4U;
    pEvent[2] = 1U;
    pEvent[3] = (uint8_t)Opcode;
    pEvent[4] = (uint8_t)(Opcode >> 8);
    pEvent[5] = Status;

    pController->ControllerPacketType = HCI_H4_PACKET_EVENT;
    pController->ControllerPacketLen = HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE;
    pController->ControllerPacketPending = true;
    return true;
}

static bool HciControllerQueueUnknownCommand(HciController_t *pController,
                                             uint16_t Opcode)
{
    return HciControllerQueueCommandStatus(
        pController, Opcode, HCI_CONTROLLER_STATUS_UNKNOWN_HCI_COMMAND);
}

static bool HciControllerQueueLoopback(HciController_t *pController,
                                       const uint8_t *pPacket,
                                       size_t PacketLen)
{
    if (pController->ControllerPacketPending)
    {
        return false;
    }

    const uint8_t paramLen = pPacket[2];
    const size_t maximumParamLen =
        HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN +
        HCI_CONTROLLER_LOOPBACK_MAX_DATA_LEN;

    if (PacketLen != HCI_CONTROLLER_COMMAND_HEADER_SIZE + (size_t)paramLen ||
        paramLen < HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN ||
        (size_t)paramLen > maximumParamLen)
    {
        return HciControllerQueueCommandStatus(
            pController,
            HCI_CONTROLLER_LOOPBACK_OPCODE,
            HCI_CONTROLLER_STATUS_INVALID_HCI_PARAMETERS);
    }

    const size_t dataLen =
        (size_t)paramLen - HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN;
    const size_t eventLen = HCI_CONTROLLER_LOOPBACK_EVENT_BASE_SIZE + dataLen;
    if (eventLen > pController->ControllerPacketCapacity)
    {
        return HciControllerQueueCommandStatus(
            pController,
            HCI_CONTROLLER_LOOPBACK_OPCODE,
            HCI_CONTROLLER_STATUS_MEMORY_CAPACITY_EXCEEDED);
    }

    const uint8_t *pParams = &pPacket[HCI_CONTROLLER_COMMAND_HEADER_SIZE];
    const uint16_t sequence = HciControllerReadLe16(pParams);
    const uint8_t seed = pParams[2];
    const uint8_t *pPayload = &pParams[HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN];

    uint8_t flags = 0U;
    uint16_t firstBad = HCI_CONTROLLER_LOOPBACK_NO_ERROR_INDEX;
    uint8_t state = seed;

    if (seed == 0U)
    {
        flags |= HCI_CONTROLLER_LOOPBACK_RX_BAD_SEED;
    }

    for (size_t i = 0U; i < dataLen; i++)
    {
        const uint8_t expected = HciControllerPrbs8Next(&state);
        if (pPayload[i] != expected &&
            firstBad == HCI_CONTROLLER_LOOPBACK_NO_ERROR_INDEX)
        {
            flags |= HCI_CONTROLLER_LOOPBACK_RX_PRBS_ERROR;
            firstBad = (uint16_t)i;
        }
    }

    uint8_t *pEvent = pController->pControllerPacket;
    uint8_t *pReturn = &pEvent[HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE];

    pEvent[0] = HCI_CONTROLLER_EVENT_COMMAND_COMPLETE;
    pEvent[1] = (uint8_t)(eventLen - 2U);
    pEvent[2] = 1U;
    pEvent[3] = (uint8_t)HCI_CONTROLLER_LOOPBACK_OPCODE;
    pEvent[4] = (uint8_t)(HCI_CONTROLLER_LOOPBACK_OPCODE >> 8);
    pEvent[5] = HCI_CONTROLLER_STATUS_SUCCESS;

    HciControllerWriteLe16(&pReturn[0], sequence);
    pReturn[2] = seed;
    pReturn[3] = flags;
    HciControllerWriteLe16(&pReturn[4], firstBad);
    HciControllerWriteLe32(&pReturn[6], HciControllerCrc32(pPayload, dataLen));
    memcpy(&pReturn[HCI_CONTROLLER_LOOPBACK_RETURN_HEADER_LEN],
           pPayload, dataLen);

    pController->ControllerPacketType = HCI_H4_PACKET_EVENT;
    pController->ControllerPacketLen = eventLen;
    pController->ControllerPacketPending = true;
    return true;
}

static bool HciControllerQueueUsbTxValidation(HciController_t *pController,
                                              const uint8_t *pPacket,
                                              size_t PacketLen)
{
    if (pController->ControllerPacketPending)
    {
        return false;
    }

    if (PacketLen != HCI_CONTROLLER_COMMAND_HEADER_SIZE || pPacket[2] != 0U)
    {
        return HciControllerQueueCommandStatus(
            pController,
            HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE,
            HCI_CONTROLLER_STATUS_INVALID_HCI_PARAMETERS);
    }

    if (pController->ControllerPacketCapacity <=
        HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE)
    {
        return HciControllerQueueCommandStatus(
            pController,
            HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE,
            HCI_CONTROLLER_STATUS_MEMORY_CAPACITY_EXCEEDED);
    }

    size_t returnCapacity =
        pController->ControllerPacketCapacity -
        HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE;
    if (returnCapacity > HCI_CONTROLLER_MAX_COMMAND_COMPLETE_RETURN_LEN)
    {
        returnCapacity = HCI_CONTROLLER_MAX_COMMAND_COMPLETE_RETURN_LEN;
    }

    uint8_t *pEvent = pController->pControllerPacket;
    uint8_t *pReturn = &pEvent[HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE];
    const size_t returnLen =
        HciUsbPlatformReadTxValidation(pReturn, returnCapacity);

    if (returnLen == 0U)
    {
        return HciControllerQueueUnknownCommand(
            pController, HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE);
    }

    if (returnLen > returnCapacity ||
        returnLen > HCI_CONTROLLER_MAX_COMMAND_COMPLETE_RETURN_LEN)
    {
        return HciControllerQueueCommandStatus(
            pController,
            HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE,
            HCI_CONTROLLER_STATUS_MEMORY_CAPACITY_EXCEEDED);
    }

    const size_t eventLen =
        HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE + returnLen;
    pEvent[0] = HCI_CONTROLLER_EVENT_COMMAND_COMPLETE;
    pEvent[1] = (uint8_t)(eventLen - 2U);
    pEvent[2] = 1U;
    pEvent[3] = (uint8_t)HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE;
    pEvent[4] = (uint8_t)(HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE >> 8);
    pEvent[5] = HCI_CONTROLLER_STATUS_SUCCESS;

    pController->ControllerPacketType = HCI_H4_PACKET_EVENT;
    pController->ControllerPacketLen = eventLen;
    pController->ControllerPacketPending = true;
    return true;
}

static void HciControllerApplyCoreProfile(HciH4PacketType_t Type,
                                          uint8_t *pPacket,
                                          size_t PacketLen)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION != 0U
    if (Type != HCI_H4_PACKET_EVENT ||
        PacketLen < HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE ||
        pPacket[0] != HCI_CONTROLLER_EVENT_COMMAND_COMPLETE ||
        pPacket[5] != 0U)
    {
        return;
    }

    const uint16_t opcode = HciControllerReadLe16(&pPacket[3]);

    if (opcode == HCI_CONTROLLER_OPCODE_READ_LOCAL_VERSION &&
        PacketLen >= HCI_CONTROLLER_LOCAL_VERSION_EVENT_SIZE)
    {
        if (pPacket[6] > HCI_CONTROLLER_TARGET_CORE_VERSION)
        {
            pPacket[6] = HCI_CONTROLLER_TARGET_CORE_VERSION;
        }
        if (pPacket[9] > HCI_CONTROLLER_TARGET_CORE_VERSION)
        {
            pPacket[9] = HCI_CONTROLLER_TARGET_CORE_VERSION;
        }
        return;
    }

#if HCI_CONTROLLER_TARGET_CORE_VERSION < HCI_CORE_VERSION_6_0
    if (opcode == HCI_CONTROLLER_OPCODE_READ_SUPPORTED_COMMANDS)
    {
        const size_t octet47 = 6U + 47U;
        if (PacketLen > octet47)
        {
            pPacket[octet47] &= (uint8_t)~0x1CU;
        }
    }
#endif
#else
    (void)Type;
    (void)pPacket;
    (void)PacketLen;
#endif
}

/* This SDC product has no Host-to-Controller SCO data path. */
static bool HciControllerHostTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_COMMAND ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_ISO;
}

static bool HciControllerOutputTypeValid(HciH4PacketType_t Type)
{
    return Type == HCI_H4_PACKET_EVENT ||
           Type == HCI_H4_PACKET_ACL ||
           Type == HCI_H4_PACKET_SCO ||
           Type == HCI_H4_PACKET_ISO;
}

static bool HciControllerH4IndicatorValid(uint8_t Type)
{
    return Type >= (uint8_t)HCI_H4_PACKET_COMMAND &&
           Type <= (uint8_t)HCI_H4_PACKET_ISO;
}

bool HciControllerPutHostPacket(HciController_t *pController,
                                HciH4PacketType_t Type,
                                const uint8_t *pPacket,
                                size_t PacketLen)
{
    if (pController == nullptr)
    {
        return false;
    }

    if (!HciControllerHostTypeValid(Type) ||
        pPacket == nullptr || PacketLen == 0U)
    {
        pController->InvalidHostPacketCount++;
        return true;
    }

    if (Type == HCI_H4_PACKET_COMMAND &&
        PacketLen >= HCI_CONTROLLER_COMMAND_HEADER_SIZE)
    {
        const uint16_t opcode = HciControllerReadLe16(pPacket);
        if (opcode == HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE)
        {
            if (!HciControllerQueueUsbTxValidation(
                    pController, pPacket, PacketLen))
            {
                pController->HostPacketRetryCount++;
                return false;
            }
            return true;
        }

        if (opcode == HCI_CONTROLLER_LOOPBACK_OPCODE)
        {
            if (!HciControllerQueueLoopback(pController, pPacket, PacketLen))
            {
                pController->HostPacketRetryCount++;
                return false;
            }
            return true;
        }

        if (!HciControllerCommandAllowedByCore(opcode))
        {
            if (!HciControllerQueueUnknownCommand(pController, opcode))
            {
                pController->HostPacketRetryCount++;
                return false;
            }
            return true;
        }
    }

    if (!pController->Controller.Put(pController->Controller.pContext,
                                     Type,
                                     pPacket,
                                     PacketLen))
    {
        pController->HostPacketRetryCount++;
        return false;
    }

    return true;
}

static bool HciControllerInitCore(HciController_t *pController,
                                  DevIntrf_t *pPacketIntrf,
                                  uint8_t *pHostPacket,
                                  size_t HostPacketCapacity,
                                  uint8_t *pControllerPacket,
                                  size_t ControllerPacketCapacity,
                                  const HciControllerOps_t *pControllerOps)
{
    if (pController == nullptr || pPacketIntrf == nullptr ||
        pHostPacket == nullptr || HostPacketCapacity == 0U ||
        HostPacketCapacity > (size_t)INT_MAX ||
        pControllerPacket == nullptr ||
        ControllerPacketCapacity < HCI_CONTROLLER_UNKNOWN_COMMAND_EVENT_SIZE ||
        ControllerPacketCapacity > (size_t)INT_MAX ||
        pControllerOps == nullptr || pControllerOps->Put == nullptr ||
        pControllerOps->Get == nullptr)
    {
        return false;
    }

    pController->pHostIntrf = pPacketIntrf;
    pController->pHostPacket = pHostPacket;
    pController->HostPacketCapacity = HostPacketCapacity;
    pController->Controller = *pControllerOps;
    pController->pControllerPacket = pControllerPacket;
    pController->ControllerPacketCapacity = ControllerPacketCapacity;
    return true;
}

bool HciControllerInitPacketTransport(HciController_t *pController,
                                      DevIntrf_t *pHostIntrf,
                                      uint8_t *pHostPacket,
                                      size_t HostPacketCapacity,
                                      uint8_t *pControllerPacket,
                                      size_t ControllerPacketCapacity,
                                      const HciControllerOps_t *pControllerOps)
{
    if (pController == nullptr)
    {
        return false;
    }

    memset(pController, 0, sizeof(*pController));
    if (!HciControllerInitCore(pController,
                               pHostIntrf,
                               pHostPacket,
                               HostPacketCapacity,
                               pControllerPacket,
                               ControllerPacketCapacity,
                               pControllerOps))
    {
        return false;
    }

    pController->HostUsesH4 = false;
    return true;
}

bool HciControllerInit(HciController_t *pController,
                       DevIntrf_t *pHostIntrf,
                       uint8_t *pHostPacket,
                       size_t HostPacketCapacity,
                       uint8_t *pControllerPacket,
                       size_t ControllerPacketCapacity,
                       const HciControllerOps_t *pControllerOps)
{
    if (pController == nullptr || pHostIntrf == nullptr ||
        pHostPacket == nullptr || HostPacketCapacity == 0U)
    {
        return false;
    }

    memset(pController, 0, sizeof(*pController));
    if (!HciIntrfTransportInitPacket(&pController->Host,
                                     pHostIntrf,
                                     pHostPacket,
                                     HostPacketCapacity))
    {
        return false;
    }

    DevIntrf_t *pPacketIntrf =
        HciIntrfTransportGetDeviceIntrf(&pController->Host);
    if (!HciControllerInitCore(pController,
                               pPacketIntrf,
                               pHostPacket,
                               HostPacketCapacity,
                               pControllerPacket,
                               ControllerPacketCapacity,
                               pControllerOps))
    {
        return false;
    }

    pController->HostUsesH4 = true;
    return true;
}

bool HciControllerUsesH4(const HciController_t *pController)
{
    return pController != nullptr && pController->HostUsesH4;
}

void HciControllerSetH4StartupResetSync(HciController_t *pController,
                                        bool Enable)
{
    if (pController == nullptr || !pController->HostUsesH4)
    {
        return;
    }

    pController->H4StartupResetSync = Enable;
    pController->H4StartupResetSyncActive = false;
    pController->H4StartupResetMatch = 0U;
}

static void HciControllerH4StartupSyncProcess(HciController_t *pController)
{
    if (pController == nullptr || !pController->H4StartupResetSyncActive)
    {
        return;
    }

    HciIntrfTransport_t *pHost = &pController->Host;

    for (uint32_t pass = 0U; pass < HCI_CONTROLLER_H4_STARTUP_RX_PASSES; pass++)
    {
        const int received = DeviceIntrfRx(pHost->pIntrf,
                                           0U,
                                           pHost->RxChunk,
                                           (int)sizeof(pHost->RxChunk));
        if (received < 0)
        {
            pHost->RxErrorCount++;
            return;
        }

        if (received == 0)
        {
            return;
        }

        if (pHost->FirstRxLen < sizeof(pHost->FirstRx))
        {
            size_t keep = (size_t)received;
            const size_t room = sizeof(pHost->FirstRx) - pHost->FirstRxLen;
            if (keep > room)
            {
                keep = room;
            }
            memcpy(&pHost->FirstRx[pHost->FirstRxLen], pHost->RxChunk, keep);
            pHost->FirstRxLen += (uint8_t)keep;
        }

        pHost->RxOctetCount += (uint32_t)received;

        size_t foundEnd = 0U;
        for (size_t i = 0U; i < (size_t)received; i++)
        {
            const uint8_t octet = pHost->RxChunk[i];
            pHost->FlushedOctetCount++;

            if (octet == s_HciControllerH4Reset[pController->H4StartupResetMatch])
            {
                pController->H4StartupResetMatch++;
            }
            else
            {
                pController->H4StartupResetMatch =
                    octet == s_HciControllerH4Reset[0] ? 1U : 0U;
            }

            if (pController->H4StartupResetMatch ==
                sizeof(s_HciControllerH4Reset))
            {
                foundEnd = i + 1U;
                break;
            }
        }

        if (foundEnd == 0U)
        {
            continue;
        }

        pHost->FlushedOctetCount -= (uint32_t)sizeof(s_HciControllerH4Reset);
        pController->H4StartupResetMatch = 0U;
        pController->H4StartupResetSyncActive = false;

        pHost->RxChunkLen = (size_t)received;
        pHost->RxChunkOffset = foundEnd;
        pHost->Open = true;

        (void)HciH4ParserFeed(&pHost->Parser,
                              s_HciControllerH4Reset,
                              sizeof(s_HciControllerH4Reset));
        return;
    }
}

static void HciControllerH4Open(HciController_t *pController)
{
    HciIntrfTransport_t *pHost = &pController->Host;

    if (pController->H4StartupResetSync)
    {
        HciIntrfTransportClose(pHost);
        pController->H4StartupResetSyncActive = true;
        pController->H4StartupResetMatch = 0U;
        return;
    }

    const int received = DeviceIntrfRx(pHost->pIntrf,
                                       0U,
                                       pHost->RxChunk,
                                       (int)sizeof(pHost->RxChunk));

    if (received > 0 && HciControllerH4IndicatorValid(pHost->RxChunk[0]))
    {
        pHost->Open = true;
        pHost->RxChunkLen = (size_t)received;
        pHost->RxChunkOffset = 0U;
        pHost->RxOctetCount += (uint32_t)received;

        if (pHost->FirstRxLen < sizeof(pHost->FirstRx))
        {
            size_t keep = (size_t)received;
            const size_t room = sizeof(pHost->FirstRx) - pHost->FirstRxLen;
            if (keep > room)
            {
                keep = room;
            }
            memcpy(&pHost->FirstRx[pHost->FirstRxLen], pHost->RxChunk, keep);
            pHost->FirstRxLen += (uint8_t)keep;
        }
        return;
    }

    if (received > 0)
    {
        pHost->FlushedOctetCount += (uint32_t)received;
    }

    HciIntrfTransportOpen(pHost);
}

static void HciControllerHostProcess(HciController_t *pController)
{
    if (pController->HostPacketPending)
    {
        if (HciControllerPutHostPacket(pController,
                                       pController->HostPacketType,
                                       pController->pHostPacket,
                                       pController->HostPacketLen))
        {
            pController->HostPacketPending = false;
            pController->HostPacketType = HCI_H4_PACKET_NONE;
            pController->HostPacketLen = 0U;
        }
        return;
    }

    /*
     * Poll every defined H:4 packet type. EVENT is not valid Host->Controller,
     * but it must still be consumed so HciControllerPutHostPacket can reject
     * and release it instead of leaving an invalid H:4 packet wedged forever.
     */
    static const HciH4PacketType_t Types[] = {
        HCI_H4_PACKET_COMMAND,
        HCI_H4_PACKET_ACL,
        HCI_H4_PACKET_SCO,
        HCI_H4_PACKET_EVENT,
        HCI_H4_PACKET_ISO,
    };

    for (size_t i = 0U; i < sizeof(Types) / sizeof(Types[0]); ++i)
    {
        const int received = DeviceIntrfRx(pController->pHostIntrf,
                                           (uint32_t)Types[i],
                                           pController->pHostPacket,
                                           (int)pController->HostPacketCapacity);
        if (received < 0)
        {
            pController->InvalidHostPacketCount++;
            return;
        }
        if (received == 0)
        {
            continue;
        }

        pController->HostPacketType = Types[i];
        pController->HostPacketLen = (size_t)received;
        pController->HostPacketPending = true;

        if (HciControllerPutHostPacket(pController,
                                       pController->HostPacketType,
                                       pController->pHostPacket,
                                       pController->HostPacketLen))
        {
            pController->HostPacketPending = false;
            pController->HostPacketType = HCI_H4_PACKET_NONE;
            pController->HostPacketLen = 0U;
        }
        return;
    }
}

static void HciControllerFetchPacket(HciController_t *pController)
{
    if (pController->ControllerPacketPending)
    {
        return;
    }

    HciH4PacketType_t type = HCI_H4_PACKET_NONE;
    size_t packetLen = 0U;
    HciControllerGetResult_t result =
        pController->Controller.Get(pController->Controller.pContext,
                                    &type,
                                    pController->pControllerPacket,
                                    pController->ControllerPacketCapacity,
                                    &packetLen);

    if (result == HCI_CONTROLLER_GET_EMPTY)
    {
        return;
    }

    if (result != HCI_CONTROLLER_GET_PACKET)
    {
        pController->ControllerGetErrorCount++;
        return;
    }

    if (!HciControllerOutputTypeValid(type) || packetLen == 0U ||
        packetLen > pController->ControllerPacketCapacity)
    {
        pController->InvalidControllerPacketCount++;
        return;
    }

    HciControllerApplyCoreProfile(type,
                                  pController->pControllerPacket,
                                  packetLen);

    if (type == HCI_H4_PACKET_ACL)
    {
        pController->ControllerAclPacketCount++;
    }

    pController->ControllerPacketType = type;
    pController->ControllerPacketLen = packetLen;
    pController->ControllerPacketPending = true;
}

static void HciControllerStartPacket(HciController_t *pController)
{
    if (!pController->ControllerPacketPending)
    {
        return;
    }

    /*
     * The H:4 adapter owns a fixed stream buffer that also has to hold the
     * packet indicator. This is a permanent size mismatch, not transport
     * backpressure, so drop/count it rather than leaving it pending forever.
     */
    if (pController->HostUsesH4 &&
        pController->ControllerPacketLen > (HCI_INTRF_TX_STREAM_SIZE - 1U))
    {
        pController->ControllerPacketPending = false;
        pController->UnsendableControllerPacketCount++;
        return;
    }

    const int sent = DeviceIntrfTx(pController->pHostIntrf,
                                   (uint32_t)pController->ControllerPacketType,
                                   pController->pControllerPacket,
                                   (int)pController->ControllerPacketLen);
    if (sent == (int)pController->ControllerPacketLen)
    {
        if (pController->ControllerPacketType == HCI_H4_PACKET_ACL)
        {
            pController->HostAclPacketCount++;
        }
        pController->ControllerPacketPending = false;
    }
    else if (sent != 0)
    {
        /* Packet transports used here must synchronously copy or refuse. */
        pController->ControllerPacketPending = false;
        pController->UnsendableControllerPacketCount++;
    }
}

void HciControllerPortOpen(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    if (pController->HostUsesH4)
    {
        HciControllerH4Open(pController);
    }
    else
    {
        DeviceIntrfEnable(pController->pHostIntrf);
    }
}

void HciControllerPortClose(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    pController->H4StartupResetSyncActive = false;
    pController->H4StartupResetMatch = 0U;

    if (pController->HostUsesH4)
    {
        HciIntrfTransportClose(&pController->Host);
    }
    else
    {
        DeviceIntrfDisable(pController->pHostIntrf);
    }

    pController->HostPacketPending = false;
    pController->HostPacketLen = 0U;
    pController->HostPacketType = HCI_H4_PACKET_NONE;
    pController->ControllerPacketPending = false;
    pController->ControllerPacketLen = 0U;
    pController->ControllerPacketType = HCI_H4_PACKET_NONE;
}

void HciControllerProcess(HciController_t *pController)
{
    if (pController == nullptr)
    {
        return;
    }

    if (pController->Controller.Process != nullptr)
    {
        pController->Controller.Process(pController->Controller.pContext);
    }

    /*
     * The H:4 adapter uses pHostPacket as its parser buffer. Do not feed it new
     * bytes while that same buffer is retained for a backend retry.
     */
    if (pController->HostUsesH4 && !pController->HostPacketPending)
    {
        HciControllerH4StartupSyncProcess(pController);
        HciIntrfTransportProcess(&pController->Host);
    }

    HciControllerHostProcess(pController);
    HciControllerFetchPacket(pController);
    HciControllerStartPacket(pController);

    if (pController->HostUsesH4 && !pController->HostPacketPending)
    {
        HciIntrfTransportProcess(&pController->Host);
    }

    HciControllerHostProcess(pController);
}

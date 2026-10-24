/**-------------------------------------------------------------------------
@file	hci_vendor.cpp
@brief	I-SYST vendor commands: static address and the crypto command gateway.

The crypto commands live in hci_vendor_crypto.cpp, which a project links to
offer them. This file holds weak defaults that answer Unsupported Feature.

@author	Nguyen Hoan Hoang
@date	October 2026
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_vendor.h"

#include <string.h>

#define HCI_VENDOR_EVENT_COMMAND_COMPLETE   0x0EU
#define HCI_VENDOR_EVENT_HDR_LEN            6U  /* code, len, ncmd, opcode, status */

extern "C" __attribute__((weak)) bool HciVendorStaticAddr(uint8_t Addr[6])
{
    (void)Addr;
    return false;
}

extern "C" __attribute__((weak)) bool HciVendorCryptoCommand(uint16_t, const uint8_t *,
        size_t, uint8_t *, size_t *, uint8_t *)
{
    return false;
}

extern "C" __attribute__((weak)) uint32_t HciVendorCryptoBits(void)
{
    return 0U;
}

extern "C" bool HciVendorIsCommand(uint16_t Opcode)
{
    return Opcode >= HCI_VENDOR_OPCODE_READ_STATIC_ADDR &&
           Opcode <= HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY;
}

/*
 * The handlers below take the command parameters and write the return
 * parameters after the status octet. They return the HCI status and set
 * *pRetLen to the return parameter count, which is 0 unless the status is
 * Success.
 */

static uint8_t HciVendorReadStaticAddr(uint8_t *pRet, size_t *pRetLen)
{
    if (!HciVendorStaticAddr(pRet))
    {
        return HCI_VENDOR_STATUS_UNSUPPORTED;
    }

    *pRetLen = 6U;
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorCryptoSupported(uint8_t *pRet, size_t *pRetLen)
{
    uint32_t bits = HciVendorCryptoBits();

    pRet[0] = (uint8_t)bits;
    pRet[1] = (uint8_t)(bits >> 8);
    pRet[2] = (uint8_t)(bits >> 16);
    pRet[3] = (uint8_t)(bits >> 24);
    *pRetLen = 4U;
    return HCI_VENDOR_STATUS_SUCCESS;
}


extern "C" size_t HciVendorCommand(const uint8_t *pPacket, size_t PacketLen,
                                   uint8_t *pEvent, size_t Capacity)
{
    if (pPacket == nullptr || pEvent == nullptr || PacketLen < 3U ||
        Capacity < HCI_VENDOR_EVENT_HDR_LEN + HCI_VENDOR_RETURN_MAX)
    {
        return 0U;
    }

    const uint16_t opcode = (uint16_t)(pPacket[0] | (pPacket[1] << 8));
    if (!HciVendorIsCommand(opcode))
    {
        return 0U;
    }

    const uint8_t *pParam = pPacket + 3U;
    const size_t paramLen = pPacket[2];
    uint8_t *pRet = pEvent + HCI_VENDOR_EVENT_HDR_LEN;
    size_t retLen = 0U;
    uint8_t status;

    if (PacketLen != 3U + paramLen)
    {
        status = HCI_VENDOR_STATUS_INVALID_PARAMS;
    }
    else
    {
        switch (opcode)
        {
            case HCI_VENDOR_OPCODE_READ_STATIC_ADDR:
                status = paramLen == 0U ? HciVendorReadStaticAddr(pRet, &retLen)
                                        : HCI_VENDOR_STATUS_INVALID_PARAMS;
                break;
            case HCI_VENDOR_OPCODE_CRYPTO_SUPPORTED:
                status = paramLen == 0U ? HciVendorCryptoSupported(pRet, &retLen)
                                        : HCI_VENDOR_STATUS_INVALID_PARAMS;
                break;
            default:
                if (!HciVendorCryptoCommand(opcode, pParam, paramLen, pRet,
                                            &retLen, &status))
                {
                    status = HCI_VENDOR_STATUS_UNSUPPORTED;
                }
                break;
        }
    }

    if (status != HCI_VENDOR_STATUS_SUCCESS)
    {
        retLen = 0U;
    }

    pEvent[0] = HCI_VENDOR_EVENT_COMMAND_COMPLETE;
    pEvent[1] = (uint8_t)(4U + retLen);
    pEvent[2] = 1U;
    pEvent[3] = (uint8_t)opcode;
    pEvent[4] = (uint8_t)(opcode >> 8);
    pEvent[5] = status;

    return HCI_VENDOR_EVENT_HDR_LEN + retLen;
}

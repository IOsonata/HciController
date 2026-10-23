/**-------------------------------------------------------------------------
@file	hci_vendor.cpp
@brief	I-SYST vendor commands: static address and controller crypto.

The crypto runs on the IOsonata engines the target already links for the
link layer: CryptoSoftAes (CMAC, CCM), CryptoSoftSha256 (SHA-256, HMAC)
and CryptoUecc (ECDSA P-256) over the entropy source HciVendorRng names.
A target with a hardware engine substitutes it behind the same facets.

@author	Nguyen Hoan Hoang
@date	October 2026
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_vendor.h"

/*
 * The crypto commands pull the software AES, SHA-256 and uECC engines into
 * the image, about 16 KB of flash. A part with little flash, such as the
 * nRF5340 network core, builds with HCI_VENDOR_CRYPTO=0. The crypto opcodes
 * then answer Unsupported Feature and Crypto Supported reports no bits.
 */
#ifndef HCI_VENDOR_CRYPTO
#define HCI_VENDOR_CRYPTO 1
#endif

#if HCI_VENDOR_CRYPTO
#include "crypto/crypto_softaes.h"
#include "crypto/crypto_softsha256.h"
#include "crypto/crypto_uecc.h"
#endif

#include <string.h>

#define HCI_VENDOR_EVENT_COMMAND_COMPLETE   0x0EU
#define HCI_VENDOR_EVENT_HDR_LEN            6U  /* code, len, ncmd, opcode, status */

#define HCI_VENDOR_STATUS_SUCCESS           0x00U
#define HCI_VENDOR_STATUS_UNSUPPORTED       0x11U  /* Unsupported Feature */
#define HCI_VENDOR_STATUS_INVALID_PARAMS    0x12U
#define HCI_VENDOR_STATUS_AUTH_FAILURE      0x05U
#define HCI_VENDOR_STATUS_HW_FAILURE        0x03U

#define AES_KEY_LEN     16U
#define AES_BLOCK_LEN   16U
#define SHA256_LEN      32U
#define P256_PRIV_LEN   32U
#define P256_PUB_LEN    64U
#define P256_SIG_LEN    64U

#if HCI_VENDOR_CRYPTO
static CryptoSoftAes s_Aes;
static CryptoSoftSha256 s_Sha;
static CryptoUecc s_Ecc;
static bool s_bEnginesReady = false;
#endif

extern "C" __attribute__((weak)) bool HciVendorStaticAddr(uint8_t Addr[6])
{
    (void)Addr;
    return false;
}

__attribute__((weak)) RngEngine *HciVendorRng(void)
{
    return nullptr;
}

#if HCI_VENDOR_CRYPTO
static void HciVendorEnginesPrepare(void)
{
    if (s_bEnginesReady)
    {
        s_Ecc.SetRng(HciVendorRng());
        return;
    }

    s_Aes.Enable();
    s_Sha.Enable();
    s_Ecc.SetRng(HciVendorRng());
    s_Ecc.Enable();
    s_bEnginesReady = true;
}

static uint32_t HciVendorSupported(void)
{
    uint32_t bits = HCI_VENDOR_CRYPTO_AES_CMAC | HCI_VENDOR_CRYPTO_AES_CCM |
                    HCI_VENDOR_CRYPTO_SHA256 | HCI_VENDOR_CRYPTO_HMAC_SHA256 |
                    HCI_VENDOR_CRYPTO_ECDSA_P256_VERIFY;

    if (HciVendorRng() != nullptr)
    {
        bits |= HCI_VENDOR_CRYPTO_RANDOM | HCI_VENDOR_CRYPTO_ECDSA_P256_SIGN;
    }

    return bits;
}
#else
static uint32_t HciVendorSupported(void)
{
    return 0U;
}
#endif

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
    uint32_t bits = HciVendorSupported();

    pRet[0] = (uint8_t)bits;
    pRet[1] = (uint8_t)(bits >> 8);
    pRet[2] = (uint8_t)(bits >> 16);
    pRet[3] = (uint8_t)(bits >> 24);
    *pRetLen = 4U;
    return HCI_VENDOR_STATUS_SUCCESS;
}

#if HCI_VENDOR_CRYPTO
static uint8_t HciVendorAesCmac(const uint8_t *pParam, size_t ParamLen,
                                uint8_t *pRet, size_t *pRetLen)
{
    if (ParamLen < HCI_VENDOR_AES_CMAC_HDR_LEN)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    size_t len = pParam[AES_KEY_LEN];
    if (ParamLen != HCI_VENDOR_AES_CMAC_HDR_LEN + len)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    CryptoKey key{CRYPTO_KEY_AES_128, CRYPTO_KEY_LOC_PLAIN, CRYPTO_KEY_USE_SIGN, {}};
    key.Plain.pData = pParam;
    key.Plain.Len = AES_KEY_LEN;

    if (s_Aes.Mac(CRYPTO_MAC_CMAC, key, pParam + HCI_VENDOR_AES_CMAC_HDR_LEN,
                  len, pRet, AES_BLOCK_LEN) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_HW_FAILURE;
    }

    *pRetLen = AES_BLOCK_LEN;
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorAesCcm(const uint8_t *pParam, size_t ParamLen,
                               uint8_t *pRet, size_t *pRetLen)
{
    /* flags, key[16], nonce_len */
    if (ParamLen < 1U + AES_KEY_LEN + 1U)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    const uint8_t flags = pParam[0];
    const uint8_t *pKey = pParam + 1U;
    size_t pos = 1U + AES_KEY_LEN;
    const size_t nonceLen = pParam[pos++];
    if (nonceLen < HCI_VENDOR_AES_CCM_NONCE_MIN ||
        nonceLen > HCI_VENDOR_AES_CCM_NONCE_MAX || ParamLen < pos + nonceLen + 1U)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }
    const uint8_t *pNonce = pParam + pos;
    pos += nonceLen;

    const size_t aadLen = pParam[pos++];
    if (ParamLen < pos + aadLen + 2U)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }
    const uint8_t *pAad = pParam + pos;
    pos += aadLen;

    const size_t tagLen = pParam[pos++];
    const size_t msgLen = pParam[pos++];
    if (tagLen < 4U || tagLen > AES_BLOCK_LEN || (tagLen & 1U) != 0U)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }
    const uint8_t *pMsg = pParam + pos;
    pos += msgLen;

    const bool decrypt = (flags & HCI_VENDOR_AES_CCM_FLAG_DECRYPT) != 0U;
    const size_t tail = decrypt ? tagLen : 0U;
    if (ParamLen != pos + tail || msgLen + (decrypt ? 0U : tagLen) > HCI_VENDOR_RETURN_MAX)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    CryptoKey key{CRYPTO_KEY_AES_128, CRYPTO_KEY_LOC_PLAIN,
                  decrypt ? CRYPTO_KEY_USE_DECRYPT : CRYPTO_KEY_USE_ENCRYPT, {}};
    key.Plain.pData = pKey;
    key.Plain.Len = AES_KEY_LEN;

    CRYPTO_STATUS st;
    if (decrypt)
    {
        st = s_Aes.Open(CRYPTO_AEAD_AES_CCM, key, pNonce, nonceLen, pAad, aadLen,
                        pMsg, msgLen, pRet, pParam + pos, tagLen);
        if (st != CRYPTO_STATUS_OK)
        {
            memset(pRet, 0, msgLen);
            return HCI_VENDOR_STATUS_AUTH_FAILURE;
        }
        *pRetLen = msgLen;
    }
    else
    {
        st = s_Aes.Seal(CRYPTO_AEAD_AES_CCM, key, pNonce, nonceLen, pAad, aadLen,
                        pMsg, msgLen, pRet, pRet + msgLen, tagLen);
        if (st != CRYPTO_STATUS_OK)
        {
            return HCI_VENDOR_STATUS_HW_FAILURE;
        }
        *pRetLen = msgLen + tagLen;
    }

    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorRandom(const uint8_t *pParam, size_t ParamLen,
                               uint8_t *pRet, size_t *pRetLen)
{
    if (ParamLen != 1U || pParam[0] == 0U || pParam[0] > HCI_VENDOR_RETURN_MAX)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    RngEngine *pRng = HciVendorRng();
    if (pRng == nullptr)
    {
        return HCI_VENDOR_STATUS_UNSUPPORTED;
    }

    if (pRng->Random(pRet, pParam[0]) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_HW_FAILURE;
    }

    *pRetLen = pParam[0];
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorSha256(const uint8_t *pParam, size_t ParamLen,
                               uint8_t *pRet, size_t *pRetLen)
{
    if (ParamLen < 1U || ParamLen != 1U + (size_t)pParam[0])
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    if (s_Sha.Hash(CRYPTO_HASH_SHA256, pParam + 1U, pParam[0], pRet) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_HW_FAILURE;
    }

    *pRetLen = SHA256_LEN;
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorHmacSha256(const uint8_t *pParam, size_t ParamLen,
                                   uint8_t *pRet, size_t *pRetLen)
{
    if (ParamLen < 2U)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    const size_t keyLen = pParam[0];
    if (ParamLen < 2U + keyLen)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }
    const size_t msgLen = pParam[1U + keyLen];
    if (ParamLen != 2U + keyLen + msgLen)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    CryptoKey key{CRYPTO_KEY_HMAC, CRYPTO_KEY_LOC_PLAIN, CRYPTO_KEY_USE_SIGN, {}};
    key.Plain.pData = pParam + 1U;
    key.Plain.Len = keyLen;

    if (s_Sha.Mac(CRYPTO_MAC_HMAC, key, pParam + 2U + keyLen, msgLen,
                  pRet, SHA256_LEN) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_HW_FAILURE;
    }

    *pRetLen = SHA256_LEN;
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorEcdsaSign(const uint8_t *pParam, size_t ParamLen,
                                  uint8_t *pRet, size_t *pRetLen)
{
    if (ParamLen != P256_PRIV_LEN + SHA256_LEN)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    if (HciVendorRng() == nullptr)
    {
        return HCI_VENDOR_STATUS_UNSUPPORTED;
    }

    CryptoKey key{CRYPTO_KEY_ECC_P256, CRYPTO_KEY_LOC_PLAIN, CRYPTO_KEY_USE_SIGN, {}};
    key.Plain.pData = pParam;
    key.Plain.Len = P256_PRIV_LEN;

    if (s_Ecc.Sign(CRYPTO_CURVE_P256, key, pParam + P256_PRIV_LEN, SHA256_LEN,
                   pRet) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_HW_FAILURE;
    }

    *pRetLen = P256_SIG_LEN;
    return HCI_VENDOR_STATUS_SUCCESS;
}

static uint8_t HciVendorEcdsaVerify(const uint8_t *pParam, size_t ParamLen)
{
    if (ParamLen != P256_PUB_LEN + SHA256_LEN + P256_SIG_LEN)
    {
        return HCI_VENDOR_STATUS_INVALID_PARAMS;
    }

    if (s_Ecc.Verify(CRYPTO_CURVE_P256, pParam, pParam + P256_PUB_LEN, SHA256_LEN,
                     pParam + P256_PUB_LEN + SHA256_LEN) != CRYPTO_STATUS_OK)
    {
        return HCI_VENDOR_STATUS_AUTH_FAILURE;
    }

    return HCI_VENDOR_STATUS_SUCCESS;
}
#endif /* HCI_VENDOR_CRYPTO */

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
#if HCI_VENDOR_CRYPTO
        HciVendorEnginesPrepare();
#endif

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
#if HCI_VENDOR_CRYPTO
            case HCI_VENDOR_OPCODE_AES_CMAC:
                status = HciVendorAesCmac(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_AES_CCM:
                status = HciVendorAesCcm(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_RANDOM:
                status = HciVendorRandom(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_SHA256:
                status = HciVendorSha256(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_HMAC_SHA256:
                status = HciVendorHmacSha256(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_ECDSA_P256_SIGN:
                status = HciVendorEcdsaSign(pParam, paramLen, pRet, &retLen);
                break;
            case HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY:
                status = HciVendorEcdsaVerify(pParam, paramLen);
                break;
#endif /* HCI_VENDOR_CRYPTO */
            default:
                status = HCI_VENDOR_STATUS_UNSUPPORTED;
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

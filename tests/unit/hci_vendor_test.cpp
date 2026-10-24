/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * The I-SYST vendor commands: static address and the controller crypto,
 * against published known answers, and their place in the bridge ahead of
 * the SoftDevice Controller.
 */

#include "hci_controller.h"
#include "hci_ctlr_fake_intrf.h"
#include "hci_vendor.h"
#include "crypto/icrypto.h"
#include "crypto/crypto.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Target hooks the test stands in for. */

static bool s_bHaveAddr = true;
static bool s_bHaveRng = true;

extern "C" bool HciVendorStaticAddr(uint8_t Addr[6])
{
    if (!s_bHaveAddr)
    {
        return false;
    }

    static const uint8_t addr[6] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0xE6U};
    memcpy(Addr, addr, 6U);
    return true;
}

/*
 * The IOsonata Crypto object takes its entropy from the MCU library. The
 * test stands in for that library: a counter source that calls itself secure,
 * and a switch that turns it off.
 */
static uint32_t s_RngState = 0x1234567U;

bool Crypto::IsSecure() const
{
    return vbValid && s_bHaveRng;
}

CRYPTO_STATUS Crypto::Random(uint8_t *pOut, size_t Len)
{
    if (!vbValid || !s_bHaveRng)
    {
        return CRYPTO_STATUS_FAIL;
    }
    for (size_t i = 0; i < Len; i++)
    {
        s_RngState = s_RngState * 1103515245U + 12345U;
        pOut[i] = (uint8_t)(s_RngState >> 16);
    }
    return CRYPTO_STATUS_OK;
}

/* Transport stand-ins the bridge test needs. */

static HciCtlrFakeIntrf_t s_Fake;

int DeviceIntrfRx(DevIntrf_t * const pDev, uint32_t DevAddr, uint8_t *pBuffer, int BufferLen)
{
    return pDev == &s_Fake.Intrf ? HciCtlrFakeRx(pDev, DevAddr, pBuffer, BufferLen) : 0;
}
int DeviceIntrfTx(DevIntrf_t * const pDev, uint32_t DevAddr, const uint8_t *pData, int DataLen)
{
    return pDev == &s_Fake.Intrf ? HciCtlrFakeTx(pDev, DevAddr, pData, DataLen) : DataLen;
}
void DeviceIntrfEnable(DevIntrf_t *) {}
void DeviceIntrfDisable(DevIntrf_t *) {}

static unsigned s_BackendPutCount;

static bool BackendPut(void *, HciH4PacketType_t, const uint8_t *, size_t)
{
    s_BackendPutCount++;
    return true;
}

static HciSdcGetResult_t BackendGet(void *, HciH4PacketType_t *,
                                            uint8_t *, size_t, size_t *)
{
    return HCI_SDC_GET_EMPTY;
}


/* Helpers. */

static uint8_t s_Event[320];

/* Runs a command and returns the status; *pRet points at the return
 * parameters and *pRetLen holds their count. */
static uint8_t Run(uint16_t Opcode, const uint8_t *pParam, size_t ParamLen,
                   const uint8_t **pRet, size_t *pRetLen)
{
    uint8_t cmd[3U + 255U];
    cmd[0] = (uint8_t)Opcode;
    cmd[1] = (uint8_t)(Opcode >> 8);
    cmd[2] = (uint8_t)ParamLen;
    if (ParamLen != 0U)
    {
        memcpy(cmd + 3U, pParam, ParamLen);
    }

    memset(s_Event, 0xA5, sizeof(s_Event));
    size_t len = HciVendorCommand(cmd, 3U + ParamLen, s_Event, sizeof(s_Event));
    assert(len >= 6U);
    assert(s_Event[0] == 0x0EU);
    assert(s_Event[1] == len - 2U);
    assert(s_Event[2] == 1U);
    assert(s_Event[3] == (uint8_t)Opcode && s_Event[4] == (uint8_t)(Opcode >> 8));
    *pRet = s_Event + 6U;
    *pRetLen = len - 6U;
    if (s_Event[5] != 0U)
    {
        assert(*pRetLen == 0U);
    }
    return s_Event[5];
}

static void TestRecognition(void)
{
    assert(!HciVendorIsCommand(0xFFF2U));
    assert(HciVendorIsCommand(0xFFF3U));
    assert(HciVendorIsCommand(0xFFFBU));
    assert(!HciVendorIsCommand(0xFFFCU));
    assert(!HciVendorIsCommand(0x2017U));
    assert(HciControllerKnowsLocalCommand(HCI_VENDOR_OPCODE_AES_CMAC, 17U));

    printf("[ok] vendor opcodes 0xFFF3 to 0xFFFB are recognized\n");
}

static void TestStaticAddr(void)
{
    const uint8_t *pRet;
    size_t retLen;

    assert(Run(HCI_VENDOR_OPCODE_READ_STATIC_ADDR, nullptr, 0U, &pRet, &retLen) == 0U);
    assert(retLen == 6U);
    assert(pRet[0] == 0x11U && pRet[5] == 0xE6U);
    assert((pRet[5] & 0xC0U) == 0xC0U);

    const uint8_t extra = 0U;
    assert(Run(HCI_VENDOR_OPCODE_READ_STATIC_ADDR, &extra, 1U, &pRet, &retLen) == 0x12U);

    s_bHaveAddr = false;
    assert(Run(HCI_VENDOR_OPCODE_READ_STATIC_ADDR, nullptr, 0U, &pRet, &retLen) == 0x11U);
    s_bHaveAddr = true;

    printf("[ok] Read Static Address answers the target address\n");
}

static void TestSupported(void)
{
    const uint8_t *pRet;
    size_t retLen;

    assert(Run(HCI_VENDOR_OPCODE_CRYPTO_SUPPORTED, nullptr, 0U, &pRet, &retLen) == 0U);
    assert(retLen == 4U);
    uint32_t bits = pRet[0] | (pRet[1] << 8) | (pRet[2] << 16) | ((uint32_t)pRet[3] << 24);
    assert(bits == (HCI_VENDOR_CRYPTO_AES_CMAC | HCI_VENDOR_CRYPTO_AES_CCM |
                    HCI_VENDOR_CRYPTO_RANDOM | HCI_VENDOR_CRYPTO_SHA256 |
                    HCI_VENDOR_CRYPTO_HMAC_SHA256 |
                    HCI_VENDOR_CRYPTO_ECDSA_P256_SIGN |
                    HCI_VENDOR_CRYPTO_ECDSA_P256_VERIFY));

    s_bHaveRng = false;
    assert(Run(HCI_VENDOR_OPCODE_CRYPTO_SUPPORTED, nullptr, 0U, &pRet, &retLen) == 0U);
    bits = pRet[0] | (pRet[1] << 8);
    assert((bits & HCI_VENDOR_CRYPTO_RANDOM) == 0U);
    assert((bits & HCI_VENDOR_CRYPTO_ECDSA_P256_SIGN) == 0U);
    assert((bits & HCI_VENDOR_CRYPTO_AES_CMAC) != 0U);
    s_bHaveRng = true;

    printf("[ok] Crypto Supported follows the entropy source\n");
}

static const uint8_t s_AesKey[16] = {
    0x2bU, 0x7eU, 0x15U, 0x16U, 0x28U, 0xaeU, 0xd2U, 0xa6U,
    0xabU, 0xf7U, 0x15U, 0x88U, 0x09U, 0xcfU, 0x4fU, 0x3cU,
};

static void TestCmac(void)
{
    const uint8_t *pRet;
    size_t retLen;
    uint8_t param[17U + 64U];

    /* RFC 4493 example 1: empty message. */
    memcpy(param, s_AesKey, 16U);
    param[16] = 0U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CMAC, param, 17U, &pRet, &retLen) == 0U);
    assert(retLen == 16U);
    static const uint8_t mac0[16] = {
        0xbbU, 0x1dU, 0x69U, 0x29U, 0xe9U, 0x59U, 0x37U, 0x28U,
        0x7fU, 0xa3U, 0x7dU, 0x12U, 0x9bU, 0x75U, 0x67U, 0x46U,
    };
    assert(memcmp(pRet, mac0, 16U) == 0);

    /* RFC 4493 example 2: one block. */
    static const uint8_t msg16[16] = {
        0x6bU, 0xc1U, 0xbeU, 0xe2U, 0x2eU, 0x40U, 0x9fU, 0x96U,
        0xe9U, 0x3dU, 0x7eU, 0x11U, 0x73U, 0x93U, 0x17U, 0x2aU,
    };
    param[16] = 16U;
    memcpy(param + 17U, msg16, 16U);
    assert(Run(HCI_VENDOR_OPCODE_AES_CMAC, param, 33U, &pRet, &retLen) == 0U);
    static const uint8_t mac16[16] = {
        0x07U, 0x0aU, 0x16U, 0xb4U, 0x6bU, 0x4dU, 0x41U, 0x44U,
        0xf7U, 0x9bU, 0xddU, 0x9dU, 0xd0U, 0x4aU, 0x28U, 0x7cU,
    };
    assert(memcmp(pRet, mac16, 16U) == 0);

    /* Length byte disagreeing with the parameter count. */
    assert(Run(HCI_VENDOR_OPCODE_AES_CMAC, param, 32U, &pRet, &retLen) == 0x12U);
    assert(Run(HCI_VENDOR_OPCODE_AES_CMAC, param, 10U, &pRet, &retLen) == 0x12U);

    printf("[ok] AES-CMAC matches RFC 4493\n");
}

static void TestCcm(void)
{
    const uint8_t *pRet;
    size_t retLen;

    /* RFC 3610 packet vector 1. */
    uint8_t key[16];
    for (unsigned i = 0; i < 16U; i++) key[i] = (uint8_t)(0xC0U + i);
    static const uint8_t nonce[13] = {
        0x00U, 0x00U, 0x00U, 0x03U, 0x02U, 0x01U, 0x00U,
        0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U,
    };
    uint8_t aad[8];
    for (unsigned i = 0; i < 8U; i++) aad[i] = (uint8_t)i;
    uint8_t msg[23];
    for (unsigned i = 0; i < 23U; i++) msg[i] = (uint8_t)(8U + i);
    static const uint8_t expect[31] = {
        0x58U, 0x8CU, 0x97U, 0x9AU, 0x61U, 0xC6U, 0x63U, 0xD2U,
        0xF0U, 0x66U, 0xD0U, 0xC2U, 0xC0U, 0xF9U, 0x89U, 0x80U,
        0x6DU, 0x5FU, 0x6BU, 0x61U, 0xDAU, 0xC3U, 0x84U, 0x17U,
        0xE8U, 0xD1U, 0x2CU, 0xFDU, 0xF9U, 0x26U, 0xE0U,
    };

    uint8_t param[128];
    size_t n = 0U;
    param[n++] = 0U;
    memcpy(param + n, key, 16U); n += 16U;
    param[n++] = 13U;
    memcpy(param + n, nonce, 13U); n += 13U;
    param[n++] = 8U;
    memcpy(param + n, aad, 8U); n += 8U;
    param[n++] = 8U;
    param[n++] = 23U;
    memcpy(param + n, msg, 23U); n += 23U;

    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, param, n, &pRet, &retLen) == 0U);
    assert(retLen == 31U);
    assert(memcmp(pRet, expect, 31U) == 0);

    /* Decrypt what came back. */
    uint8_t dparam[128];
    size_t d = 0U;
    dparam[d++] = HCI_VENDOR_AES_CCM_FLAG_DECRYPT;
    memcpy(dparam + d, key, 16U); d += 16U;
    dparam[d++] = 13U;
    memcpy(dparam + d, nonce, 13U); d += 13U;
    dparam[d++] = 8U;
    memcpy(dparam + d, aad, 8U); d += 8U;
    dparam[d++] = 8U;
    dparam[d++] = 23U;
    memcpy(dparam + d, expect, 31U); d += 31U;

    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, dparam, d, &pRet, &retLen) == 0U);
    assert(retLen == 23U);
    assert(memcmp(pRet, msg, 23U) == 0);

    /* A damaged tag. */
    dparam[d - 1U] ^= 0x01U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, dparam, d, &pRet, &retLen) == 0x05U);
    dparam[d - 1U] ^= 0x01U;

    /* Nonce too short, tag odd, count short. */
    param[17] = 6U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, param, n, &pRet, &retLen) == 0x12U);
    param[17] = 13U;
    param[40] = 7U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, param, n, &pRet, &retLen) == 0x12U);
    param[40] = 8U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, param, n - 1U, &pRet, &retLen) == 0x12U);

    printf("[ok] AES-CCM matches RFC 3610 both ways\n");
}

static void TestRandom(void)
{
    const uint8_t *pRet;
    size_t retLen;
    uint8_t len = 32U;

    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0U);
    assert(retLen == 32U);
    uint8_t first[32];
    memcpy(first, pRet, 32U);
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0U);
    assert(memcmp(first, pRet, 32U) != 0);

    len = 251U;
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0U);
    assert(retLen == 251U);
    len = 252U;
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0x12U);
    len = 0U;
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0x12U);

    s_bHaveRng = false;
    len = 8U;
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, &len, 1U, &pRet, &retLen) == 0x11U);
    s_bHaveRng = true;

    printf("[ok] Random draws up to 251 octets from the target source\n");
}

static void TestSha(void)
{
    const uint8_t *pRet;
    size_t retLen;
    uint8_t param[64];

    param[0] = 3U;
    memcpy(param + 1U, "abc", 3U);
    assert(Run(HCI_VENDOR_OPCODE_SHA256, param, 4U, &pRet, &retLen) == 0U);
    assert(retLen == 32U);
    static const uint8_t abc[32] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
    };
    assert(memcmp(pRet, abc, 32U) == 0);
    assert(Run(HCI_VENDOR_OPCODE_SHA256, param, 5U, &pRet, &retLen) == 0x12U);

    /* RFC 4231 test case 2. */
    static const char msg[] = "what do ya want for nothing?";
    param[0] = 4U;
    memcpy(param + 1U, "Jefe", 4U);
    param[5] = 28U;
    memcpy(param + 6U, msg, 28U);
    assert(Run(HCI_VENDOR_OPCODE_HMAC_SHA256, param, 34U, &pRet, &retLen) == 0U);
    assert(retLen == 32U);
    static const uint8_t hmac[32] = {
        0x5bU, 0xdcU, 0xc1U, 0x46U, 0xbfU, 0x60U, 0x75U, 0x4eU,
        0x6aU, 0x04U, 0x24U, 0x26U, 0x08U, 0x95U, 0x75U, 0xc7U,
        0x5aU, 0x00U, 0x3fU, 0x08U, 0x9dU, 0x27U, 0x39U, 0x83U,
        0x9dU, 0xecU, 0x58U, 0xb9U, 0x64U, 0xecU, 0x38U, 0x43U,
    };
    assert(memcmp(pRet, hmac, 32U) == 0);
    assert(Run(HCI_VENDOR_OPCODE_HMAC_SHA256, param, 33U, &pRet, &retLen) == 0x12U);
    assert(Run(HCI_VENDOR_OPCODE_HMAC_SHA256, param, 1U, &pRet, &retLen) == 0x12U);

    printf("[ok] SHA-256 and HMAC-SHA-256 match FIPS 180-4 and RFC 4231\n");
}

static void TestEcdsa(void)
{
    const uint8_t *pRet;
    size_t retLen;

    /* RFC 6979 A.2.5 P-256 key pair. */
    static const uint8_t priv[32] = {
        0xC9U, 0xAFU, 0xA9U, 0xD8U, 0x45U, 0xBAU, 0x75U, 0x16U,
        0x6BU, 0x5CU, 0x21U, 0x57U, 0x67U, 0xB1U, 0xD6U, 0x93U,
        0x4EU, 0x50U, 0xC3U, 0xDBU, 0x36U, 0xE8U, 0x9BU, 0x12U,
        0x7BU, 0x8AU, 0x62U, 0x2BU, 0x12U, 0x0FU, 0x67U, 0x21U,
    };
    static const uint8_t pub[64] = {
        0x60U, 0xFEU, 0xD4U, 0xBAU, 0x25U, 0x5AU, 0x9DU, 0x31U,
        0xC9U, 0x61U, 0xEBU, 0x74U, 0xC6U, 0x35U, 0x6DU, 0x68U,
        0xC0U, 0x49U, 0xB8U, 0x92U, 0x3BU, 0x61U, 0xFAU, 0x6CU,
        0xE6U, 0x69U, 0x62U, 0x2EU, 0x60U, 0xF2U, 0x9FU, 0xB6U,
        0x79U, 0x03U, 0xFEU, 0x10U, 0x08U, 0xB8U, 0xBCU, 0x99U,
        0xA4U, 0x1AU, 0xE9U, 0xE9U, 0x56U, 0x28U, 0xBCU, 0x64U,
        0xF2U, 0xF1U, 0xB2U, 0x0CU, 0x2DU, 0x7EU, 0x9FU, 0x51U,
        0x77U, 0xA3U, 0xC2U, 0x94U, 0xD4U, 0x46U, 0x22U, 0x99U,
    };
    /* SHA-256("sample") */
    static const uint8_t hash[32] = {
        0xafU, 0x2bU, 0xdbU, 0xe1U, 0xaaU, 0x9bU, 0x6eU, 0xc1U,
        0xe2U, 0xadU, 0xe1U, 0xd6U, 0x94U, 0xf4U, 0x1fU, 0xc7U,
        0x1aU, 0x83U, 0x1dU, 0x02U, 0x68U, 0xe9U, 0x89U, 0x15U,
        0x62U, 0x11U, 0x3dU, 0x8aU, 0x62U, 0xadU, 0xd1U, 0xbfU,
    };
    /* RFC 6979 A.2.5 deterministic signature for that message; the
     * verify side checks it, the sign side is randomized. */
    static const uint8_t rfcSig[64] = {
        0xEFU, 0xD4U, 0x8BU, 0x2AU, 0xACU, 0xB6U, 0xA8U, 0xFDU,
        0x11U, 0x40U, 0xDDU, 0x9CU, 0xD4U, 0x5EU, 0x81U, 0xD6U,
        0x9DU, 0x2CU, 0x87U, 0x7BU, 0x56U, 0xAAU, 0xF9U, 0x91U,
        0xC3U, 0x4DU, 0x0EU, 0xA8U, 0x4EU, 0xAFU, 0x37U, 0x16U,
        0xF7U, 0xCBU, 0x1CU, 0x94U, 0x2DU, 0x65U, 0x7CU, 0x41U,
        0xD4U, 0x36U, 0xC7U, 0xA1U, 0xB6U, 0xE2U, 0x9FU, 0x65U,
        0xF3U, 0xE9U, 0x00U, 0xDBU, 0xB9U, 0xAFU, 0xF4U, 0x06U,
        0x4DU, 0xC4U, 0xABU, 0x2FU, 0x84U, 0x3AU, 0xCDU, 0xA8U,
    };

    uint8_t vparam[64U + 32U + 64U];
    memcpy(vparam, pub, 64U);
    memcpy(vparam + 64U, hash, 32U);
    memcpy(vparam + 96U, rfcSig, 64U);
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY, vparam, sizeof(vparam),
               &pRet, &retLen) == 0U);
    assert(retLen == 0U);
    vparam[100] ^= 0x01U;
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY, vparam, sizeof(vparam),
               &pRet, &retLen) == 0x05U);
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY, vparam, 100U,
               &pRet, &retLen) == 0x12U);

    uint8_t sparam[64];
    memcpy(sparam, priv, 32U);
    memcpy(sparam + 32U, hash, 32U);
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_SIGN, sparam, 64U, &pRet, &retLen) == 0U);
    assert(retLen == 64U);
    memcpy(vparam + 96U, pRet, 64U);
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY, vparam, sizeof(vparam),
               &pRet, &retLen) == 0U);
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_SIGN, sparam, 63U, &pRet, &retLen) == 0x12U);

    s_bHaveRng = false;
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_SIGN, sparam, 64U, &pRet, &retLen) == 0x11U);
    s_bHaveRng = true;

    printf("[ok] ECDSA P-256 verifies RFC 6979 and its own signatures\n");
}

static void TestBridge(void)
{
    struct {
        HciController_t Controller;
        DevIntrf_t Host;
        uint8_t HostPacket[64];
        uint8_t ControllerPacket[320];
    } f;
    memset(&f, 0, sizeof(f));
    s_BackendPutCount = 0U;

    DevIntrf_t *ctlr = HciCtlrFakeIntrfInit(&s_Fake, BackendPut, BackendGet, NULL);
    assert(HciControllerInitPacketTransport(&f.Controller, &f.Host,
                                            f.HostPacket, sizeof(f.HostPacket),
                                            f.ControllerPacket,
                                            sizeof(f.ControllerPacket), ctlr));

    const uint8_t cmd[] = {0xF3U, 0xFFU, 0x00U};
    assert(HciControllerPutHostPacket(&f.Controller, HCI_H4_PACKET_COMMAND,
                                      cmd, sizeof(cmd)));
    assert(s_BackendPutCount == 0U);
    assert(f.Controller.ControllerPacketPending);
    assert(f.Controller.ControllerPacketType == HCI_H4_PACKET_EVENT);
    assert(f.Controller.ControllerPacketLen == 12U);
    assert(f.ControllerPacket[0] == 0x0EU && f.ControllerPacket[5] == 0U);
    assert(f.ControllerPacket[6] == 0x11U && f.ControllerPacket[11] == 0xE6U);

    /* A second one waits while the first is pending. */
    assert(!HciControllerPutHostPacket(&f.Controller, HCI_H4_PACKET_COMMAND,
                                       cmd, sizeof(cmd)));
    assert(f.Controller.HostPacketRetryCount == 1U);

    /* The opcode after ours goes on to the controller. */
    f.Controller.ControllerPacketPending = false;
    const uint8_t other[] = {0xFCU, 0xFFU, 0x00U};
    assert(HciControllerPutHostPacket(&f.Controller, HCI_H4_PACKET_COMMAND,
                                      other, sizeof(other)));
    assert(s_BackendPutCount == 1U);

    printf("[ok] the bridge answers vendor commands ahead of the controller\n");
}

/* The smallest well formed request of each command, as the Python catalog
 * sends them in the broad hardware probe, must come back Success. */
static void TestProbes(void)
{
    const uint8_t *pRet;
    size_t retLen;
    uint8_t p[192];

    memset(p, 0, sizeof(p));
    assert(Run(HCI_VENDOR_OPCODE_AES_CMAC, p, 17U, &pRet, &retLen) == 0U);

    /* flags 0, key[16], nonce_len 13, nonce[13], aad_len 0, tag_len 4, msg_len 0 */
    memset(p, 0, sizeof(p));
    p[17] = 13U;
    p[31] = 0U;
    p[32] = 4U;
    p[33] = 0U;
    assert(Run(HCI_VENDOR_OPCODE_AES_CCM, p, 34U, &pRet, &retLen) == 0U);
    assert(retLen == 4U);

    p[0] = 8U;
    assert(Run(HCI_VENDOR_OPCODE_RANDOM, p, 1U, &pRet, &retLen) == 0U);
    p[0] = 0U;
    assert(Run(HCI_VENDOR_OPCODE_SHA256, p, 1U, &pRet, &retLen) == 0U);
    p[1] = 0U;
    assert(Run(HCI_VENDOR_OPCODE_HMAC_SHA256, p, 2U, &pRet, &retLen) == 0U);

    memset(p, 0, sizeof(p));
    p[31] = 2U;
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_SIGN, p, 64U, &pRet, &retLen) == 0U);
    memset(p, 0, sizeof(p));
    assert(Run(HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY, p, 160U, &pRet, &retLen) == 0x05U);

    printf("[ok] the catalog probes answer as the harness expects\n");
}

int main(void)
{
    TestRecognition();
    TestStaticAddr();
    TestSupported();
    TestCmac();
    TestCcm();
    TestRandom();
    TestSha();
    TestEcdsa();
    TestBridge();
    TestProbes();
    printf("hci_vendor_test passed\n");
    return 0;
}

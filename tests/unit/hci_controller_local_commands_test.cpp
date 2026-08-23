/*
 * Copyright (c) 2026 I-SYST inc.
 * SPDX-License-Identifier: MPL-2.0
 *
 * Focused coverage for HciController vendor commands that are consumed by the
 * bridge before the request reaches the SDC dispatch tables.
 */

#include "hci_controller.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned s_BackendPutCount;
static bool s_ValidationAvailable = true;

extern "C" size_t HciUsbPlatformReadTxValidation(uint8_t *pData,
                                                  size_t Capacity)
{
    if (!s_ValidationAvailable || pData == nullptr || Capacity < 2U)
    {
        return 0U;
    }

    pData[0] = 1U;
    pData[1] = 0U;
    return 2U;
}

int DeviceIntrfRx(DevIntrf_t * const,
                  uint32_t,
                  uint8_t *,
                  int)
{
    return 0;
}

int DeviceIntrfTx(DevIntrf_t * const,
                  uint32_t,
                  const uint8_t *,
                  int DataLen)
{
    return DataLen;
}

void DeviceIntrfEnable(DevIntrf_t *)
{
}

void DeviceIntrfDisable(DevIntrf_t *)
{
}

static bool BackendPut(void *,
                       HciH4PacketType_t,
                       const uint8_t *,
                       size_t)
{
    s_BackendPutCount++;
    return true;
}

static HciControllerGetResult_t BackendGet(void *,
                                            HciH4PacketType_t *,
                                            uint8_t *,
                                            size_t,
                                            size_t *)
{
    return HCI_CONTROLLER_GET_EMPTY;
}

static void BackendProcess(void *)
{
}

typedef struct
{
    HciController_t Controller;
    DevIntrf_t Host;
    uint8_t HostPacket[64];
    uint8_t ControllerPacket[320];
} Fixture_t;

static void Setup(Fixture_t *pFixture)
{
    memset(pFixture, 0, sizeof(*pFixture));
    s_BackendPutCount = 0U;
    s_ValidationAvailable = true;

    HciControllerOps_t ops = {};
    ops.Put = BackendPut;
    ops.Get = BackendGet;
    ops.Process = BackendProcess;

    assert(HciControllerInitPacketTransport(&pFixture->Controller,
                                            &pFixture->Host,
                                            pFixture->HostPacket,
                                            sizeof(pFixture->HostPacket),
                                            pFixture->ControllerPacket,
                                            sizeof(pFixture->ControllerPacket),
                                            &ops));
}

static void ClearPending(Fixture_t *pFixture)
{
    pFixture->Controller.ControllerPacketPending = false;
    pFixture->Controller.ControllerPacketLen = 0U;
    pFixture->Controller.ControllerPacketType = HCI_H4_PACKET_NONE;
}

static void TestLocalRecognition(void)
{
    assert(HciControllerKnowsLocalCommand(HCI_CONTROLLER_LOOPBACK_OPCODE,
                                          HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN));
    assert(HciControllerKnowsLocalCommand(
        HCI_CONTROLLER_LOOPBACK_OPCODE,
        HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN +
            HCI_CONTROLLER_LOOPBACK_MAX_DATA_LEN));
    assert(!HciControllerKnowsLocalCommand(HCI_CONTROLLER_LOOPBACK_OPCODE,
                                           HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN - 1U));
    assert(HciControllerKnowsLocalCommand(
        HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE, 0U));
    assert(!HciControllerKnowsLocalCommand(
        HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE, 1U));

    printf("[ok] bridge-local vendor command lengths are recognized\n");
}

static void TestLoopback(void)
{
    Fixture_t fixture;
    Setup(&fixture);

    const uint8_t command[] = {
        0xF1U, 0xFFU, 0x03U,
        0x34U, 0x12U, 0x5AU,
    };

    assert(HciControllerPutHostPacket(&fixture.Controller,
                                      HCI_H4_PACKET_COMMAND,
                                      command,
                                      sizeof(command)));
    assert(s_BackendPutCount == 0U);
    assert(fixture.Controller.ControllerPacketPending);
    assert(fixture.Controller.ControllerPacketType == HCI_H4_PACKET_EVENT);
    assert(fixture.Controller.ControllerPacketLen == 16U);

    const uint8_t *pEvent = fixture.ControllerPacket;
    assert(pEvent[0] == 0x0EU);
    assert(pEvent[1] == 14U);
    assert(pEvent[2] == 1U);
    assert(pEvent[3] == 0xF1U && pEvent[4] == 0xFFU);
    assert(pEvent[5] == 0U);
    assert(pEvent[6] == 0x34U && pEvent[7] == 0x12U);
    assert(pEvent[8] == 0x5AU);
    assert(pEvent[9] == 0U);
    assert(pEvent[10] == 0xFFU && pEvent[11] == 0xFFU);
    assert(pEvent[12] == 0U && pEvent[13] == 0U &&
           pEvent[14] == 0U && pEvent[15] == 0U);

    printf("[ok] 0xFFF1 loopback completes locally without reaching SDC\n");
}

static void TestUsbValidation(void)
{
    Fixture_t fixture;
    Setup(&fixture);

    const uint8_t command[] = {0xF2U, 0xFFU, 0x00U};
    assert(HciControllerPutHostPacket(&fixture.Controller,
                                      HCI_H4_PACKET_COMMAND,
                                      command,
                                      sizeof(command)));
    assert(s_BackendPutCount == 0U);
    assert(fixture.Controller.ControllerPacketPending);
    assert(fixture.Controller.ControllerPacketLen == 8U);

    const uint8_t *pEvent = fixture.ControllerPacket;
    assert(pEvent[0] == 0x0EU);
    assert(pEvent[1] == 6U);
    assert(pEvent[2] == 1U);
    assert(pEvent[3] == 0xF2U && pEvent[4] == 0xFFU);
    assert(pEvent[5] == 0U);
    assert(pEvent[6] == 1U && pEvent[7] == 0U);

    ClearPending(&fixture);
    s_ValidationAvailable = false;
    assert(HciControllerPutHostPacket(&fixture.Controller,
                                      HCI_H4_PACKET_COMMAND,
                                      command,
                                      sizeof(command)));
    assert(fixture.Controller.ControllerPacketLen == 6U);
    assert(fixture.ControllerPacket[5] == 0x01U);

    ClearPending(&fixture);
    s_ValidationAvailable = true;
    const uint8_t invalid[] = {0xF2U, 0xFFU, 0x01U, 0x00U};
    assert(HciControllerPutHostPacket(&fixture.Controller,
                                      HCI_H4_PACKET_COMMAND,
                                      invalid,
                                      sizeof(invalid)));
    assert(fixture.Controller.ControllerPacketLen == 6U);
    assert(fixture.ControllerPacket[5] == 0x12U);

    printf("[ok] 0xFFF2 returns snapshot, unavailable and invalid-parameter status\n");
}

int main(void)
{
    TestLocalRecognition();
    TestLoopback();
    TestUsbValidation();

    printf("All HciController local-command tests passed.\n");
    return 0;
}

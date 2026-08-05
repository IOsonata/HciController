/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_CONTROLLER_H
#define HCI_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_intrf_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HCI_CONTROLLER_GET_ERROR  = -1,
    HCI_CONTROLLER_GET_EMPTY  = 0,
    HCI_CONTROLLER_GET_PACKET = 1,
} HciControllerGetResult_t;

typedef bool (*HciControllerPut_t)(void *pContext,
                                   HciH4PacketType_t Type,
                                   const uint8_t *pPacket,
                                   size_t PacketLen);

typedef HciControllerGetResult_t (*HciControllerGet_t)(void *pContext,
                                                        HciH4PacketType_t *pType,
                                                        uint8_t *pPacket,
                                                        size_t PacketCapacity,
                                                        size_t *pPacketLen);

typedef void (*HciControllerProcess_t)(void *pContext);

typedef struct {
    HciControllerPut_t Put;
    HciControllerGet_t Get;
    HciControllerProcess_t Process;
    void *pContext;
} HciControllerOps_t;

typedef struct {
    HciIntrfTransport_t Host;
    HciControllerOps_t Controller;

    uint8_t *pControllerPacket;
    size_t ControllerPacketCapacity;
    size_t ControllerPacketLen;
    HciH4PacketType_t ControllerPacketType;
    bool ControllerPacketPending;

    uint32_t HostPacketRetryCount;
    uint32_t InvalidHostPacketCount;
    uint32_t ControllerGetErrorCount;
    uint32_t InvalidControllerPacketCount;
    uint32_t UnsendableControllerPacketCount;
} HciController_t;

bool HciControllerInit(HciController_t *pController,
                       DevIntrf_t *pHostIntrf,
                       uint8_t *pHostPacket,
                       size_t HostPacketCapacity,
                       uint8_t *pControllerPacket,
                       size_t ControllerPacketCapacity,
                       const HciControllerOps_t *pControllerOps);

void HciControllerPortOpen(HciController_t *pController);
void HciControllerPortClose(HciController_t *pController);
void HciControllerProcess(HciController_t *pController);

#ifdef __cplusplus
}
#endif

#endif /* HCI_CONTROLLER_H */

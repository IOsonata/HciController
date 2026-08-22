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

/*
 * HciController transport integrity loopback.
 *
 * This is an HCI vendor command rather than a USB diagnostic, so the same
 * command exercises UART/CDC H:4 and native Bluetooth USB. The request carries
 * a sequence number, a non-zero PRBS8 seed and 0..240 generated octets. The
 * Command Complete returns controller-side validation information followed by
 * the exact bytes the controller received.
 *
 * Request parameters:
 *   0..1  sequence, little endian
 *   2     PRBS8 seed
 *   3..   PRBS payload
 *
 * Return parameters after the ordinary HCI status byte:
 *   0..1  received sequence, little endian
 *   2     received seed
 *   3     RX flags
 *   4..5  first PRBS mismatch, little endian; 0xFFFF when none
 *   6..9  CRC-32/ISO-HDLC of the payload the controller received
 *   10..  exact received payload
 *
 * With N payload octets the complete Command Complete Event is 16+N octets.
 * Sweeping N=0..240 therefore exercises every Event-IN length from 16 through
 * 256 octets, including every nRF52840 16-byte interrupt-IN boundary.
 */
#define HCI_CONTROLLER_LOOPBACK_OPCODE             0xFFF1U
#define HCI_CONTROLLER_LOOPBACK_REQUEST_HEADER_LEN 3U
#define HCI_CONTROLLER_LOOPBACK_RETURN_HEADER_LEN  10U
#define HCI_CONTROLLER_LOOPBACK_MAX_DATA_LEN       240U
#define HCI_CONTROLLER_LOOPBACK_RX_PRBS_ERROR      0x01U
#define HCI_CONTROLLER_LOOPBACK_RX_BAD_SEED        0x02U
#define HCI_CONTROLLER_LOOPBACK_NO_ERROR_INDEX     0xFFFFU

/*
 * Native USB pre-transmit validation snapshot.
 *
 * This diagnostic is handled locally by the bridge so it can snapshot the USB
 * transport without passing through SDC. The platform hook returns an opaque
 * diagnostic payload whose format is owned by the USB implementation and the
 * matching host diagnostic. A non-USB build has no strong platform hook and
 * answers Unknown HCI Command.
 *
 * The current native-USB payload starts with version/count and then carries the
 * last eight Event-IN records. Each record contains a monotonically increasing
 * sequence, HCI Event length, structural-validation flags, event code,
 * CRC-32/ISO-HDLC, the first eight bytes and the last eight bytes. The snapshot
 * is taken before this command's own Command Complete enters USB, so the reply
 * cannot overwrite the evidence it reports.
 */
#define HCI_CONTROLLER_USB_TX_VALIDATION_OPCODE 0xFFF2U

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
    /*
     * Packet-facing host interface used by the controller in every mode.
     * DevAddr is HciH4PacketType_t and pData contains only the HCI packet.
     *
     * Native USB implements this directly. UART/CDC are wrapped by Host below,
     * which converts their H:4 byte stream into the same packet DeviceIntrf.
     */
    DevIntrf_t *pHostIntrf;
    HciIntrfTransport_t Host;
    bool HostUsesH4;

    /*
     * Optional UART startup synchronization. Some embedded hosts print boot
     * text on the HCI UART before issuing HCI Reset. While active, raw bytes
     * are discarded until the complete H:4 Reset frame 01 03 0C 00 is found.
     * The mode is opt-in so CDC/H:4 keeps its ordinary framing behavior.
     */
    bool H4StartupResetSync;
    bool H4StartupResetSyncActive;
    uint8_t H4StartupResetMatch;

    /* Packet retained here while the controller backend applies backpressure. */
    uint8_t *pHostPacket;
    size_t HostPacketCapacity;
    size_t HostPacketLen;
    HciH4PacketType_t HostPacketType;
    bool HostPacketPending;

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

    /* Successful Controller-to-Host ACL checkpoints for transport diagnosis. */
    uint32_t ControllerAclPacketCount;
    uint32_t HostAclPacketCount;
} HciController_t;

/*
 * Build a packet DeviceIntrf over an H:4 byte-stream DeviceIntrf, then use the
 * same packet controller path as every other host transport.
 */
bool HciControllerInit(HciController_t *pController,
                       DevIntrf_t *pHostIntrf,
                       uint8_t *pHostPacket,
                       size_t HostPacketCapacity,
                       uint8_t *pControllerPacket,
                       size_t ControllerPacketCapacity,
                       const HciControllerOps_t *pControllerOps);

/*
 * Use an already packet-oriented DeviceIntrf such as native Bluetooth USB.
 *
 * Packet DeviceIntrf convention:
 *   DevAddr = HciH4PacketType_t
 *   pData   = HCI packet bytes only, without an H:4 indicator
 *   one successful Rx/Tx = one complete HCI packet
 */
bool HciControllerInitPacketTransport(HciController_t *pController,
                                      DevIntrf_t *pHostIntrf,
                                      uint8_t *pHostPacket,
                                      size_t HostPacketCapacity,
                                      uint8_t *pControllerPacket,
                                      size_t ControllerPacketCapacity,
                                      const HciControllerOps_t *pControllerOps);

bool HciControllerPutHostPacket(HciController_t *pController,
                                HciH4PacketType_t Type,
                                const uint8_t *pPacket,
                                size_t PacketLen);

/* Used by H:4 resynchronization code that must distinguish a real local HCI
 * command from arbitrary bytes before dispatching it. */
bool HciControllerKnowsLocalCommand(uint16_t Opcode, size_t ParamLen);

bool HciControllerUsesH4(const HciController_t *pController);
void HciControllerSetH4StartupResetSync(HciController_t *pController,
                                        bool Enable);

void HciControllerPortOpen(HciController_t *pController);
void HciControllerPortClose(HciController_t *pController);
void HciControllerProcess(HciController_t *pController);

#ifdef __cplusplus
}
#endif

#endif

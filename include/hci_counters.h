/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_COUNTERS_H
#define HCI_COUNTERS_H

#include "hci_controller.h"
#include "hci_sdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Vendor specific readout of every counter the firmware keeps, across all four
 * layers: the H:4 parser, the transport, the bridge and the SDC routing layer.
 * None of it was on the wire, so a running board could only be questioned with
 * a debugger, which is no use on a sealed dongle.
 *
 * Spreading the counters over four structures is what made the first version
 * of this readout misleading. It reported the SDC layer alone, and every one
 * of those is a failure counter, so a flood of ACL that moved nothing could
 * not be told apart from a flood that never reached the SDC layer at all. The
 * readout now spans the stack and counts successes as well as refusals, so
 * silence means what it appears to mean.
 *
 * Vol 4 Part E 5.4.1 leaves opcode group 0x3F to the vendor. The default sits
 * at the top of that group, clear of the ones the SoftDevice Controller
 * defines for itself, which stop at 0xFC1F. Override it if it ever collides.
 */
#ifndef HCI_COUNTERS_OPCODE
#define HCI_COUNTERS_OPCODE 0xFFF0U
#endif

/*
 * One version byte then the counters, each four octets little endian, in the
 * order below. The version goes first so a host can tell an older controller
 * from a newer one rather than inferring it from the length. Counters are
 * appended and the version raised; renumbering would make an older host read
 * the wrong field and report a fault that is not there.
 *
 * Version 1 carried indices 0 to 15, version 2 appends 16 to 29, and version 3
 * appends 30 and 31.
 *
 *   0  CommandCount                     commands the dispatch table accepted
 *   1  UnknownCommandCount              opcodes with no entry
 *   2  InvalidPacketCount               header length disagreed with the packet
 *   3  InvalidParamLenCount             parameter length disagreed with the entry
 *   4  HandlerErrorCount                handler produced nothing usable
 *   5  EventBackpressureCount           a response had nowhere to go
 *   6  AclPutErrorCount                 controller refused an ACL packet
 *   7  IsoPutErrorCount                 controller refused an ISO packet
 *   8  PutRetryCount                    controller asked for a retry
 *   9  GetErrorCount                    controller queue returned an error
 *  10  InvalidOutputTypeCount           controller named an unknown packet type
 *  11  InvalidOutputLengthCount         controller packet did not fit its header
 *  12  CommandDeferredCount             command response waited for the queue
 *  13  AclOversizeCount                 host exceeded the advertised ACL size
 *  14  IsoDropCount                     ISO packet dropped
 *  15  CreditOverflowCount              more handles owed credits than tracked
 *  16  AclPutCount                      ACL packets the controller took
 *  17  IsoPutCount                      ISO packets the controller took
 *  18  H4InvalidTypeCount               packet indicator the parser rejected
 *  19  H4OversizePacketCount            packet larger than the parser buffer
 *  20  H4DeliveryRetryCount             parsed packet offered again
 *  21  RxErrorCount                     transport read failed
 *  22  TxErrorCount                     transport write failed
 *  23  TxBusyCount                      transport write deferred
 *  24  TxOversizeCount                  packet too large for the tx stream
 *  25  HostPacketRetryCount             host packet offered to the controller again
 *  26  InvalidHostPacketCount           host packet the bridge rejected
 *  27  ControllerGetErrorCount          controller queue error seen by the bridge
 *  28  InvalidControllerPacketCount     controller packet the bridge rejected
 *  29  UnsendableControllerPacketCount  controller packet the host would not take
 *  30  AclCreditOverrunCount            host exceeded the buffers it was told
 *  31  AclTrackOverflowCount            more links in flight than tracked
 */
#define HCI_COUNTERS_VERSION    3U
#define HCI_COUNTERS_COUNT      32U
#define HCI_COUNTERS_RETURN_LEN (1U + (HCI_COUNTERS_COUNT * 4U))

/*
 * The layers to read. Either may be NULL, in which case its counters report
 * zero rather than the readout refusing, so a partly built stack can still be
 * questioned. The transport and the parser are reached through the bridge.
 */
typedef struct {
    /*
     * Not const: the LE Read Buffer Size handler reaches the routing layer
     * through here to record the buffer count the controller answered with.
     */
    HciSdc_t *pSdc;
    const HciController_t *pController;
} HciCounters_t;

void HciCountersInit(HciCounters_t *pCounters,
                     HciSdc_t *pSdc,
                     const HciController_t *pController);

/*
 * Command handler. The command context has to be the HciCounters_t; given
 * anything else it answers Command Disallowed rather than reading whatever it
 * was passed.
 */
HciCmdResult_t HciCountersRead(void *pContext,
                               const uint8_t *pParams,
                               size_t ParamLen,
                               uint8_t *pReturn,
                               size_t ReturnCapacity);

#ifdef __cplusplus
}
#endif

#endif /* HCI_COUNTERS_H */

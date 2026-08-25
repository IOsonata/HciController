/**-------------------------------------------------------------------------
@file	hci_counters.h

@brief	HciController diagnostic counter readout definitions.

		Defines the vendor-specific counter command, stable counter schema,
		platform diagnostic range, and stack references used to expose
		controller, transport, SDC, and USB diagnostics to the host.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

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
 * Version 1 carried indices 0 to 15, version 2 appends 16 to 29, version 3
 * appends 30 and 31, version 4 appends 32 and 33, version 5 appends the
 * four PAwR delayed-completion checkpoints at 34 to 37, version 6 appends
 * Controller-to-Host ACL success checkpoints at 38 and 39, version 7 appends
 * the nRF52840 legacy USB Event-IN checkpoints at 40 to 48, and version 8
 * appends thirteen packed EP1 acknowledgement trace words at 49 to 61.
 *
 * Indices 32 and 33 are not counters. They are the two numbers that decide
 * whether the controller starts at all, and they are here for the reason the
 * rest of this readout exists: a sealed dongle has no console, so the trace
 * that shows them on a development board reaches nobody on a shipped one.
 *
 * The pool is computed from the configuration in hci_nrf52840.h, but the
 * figure that actually decides is the one sdc_cfg_set answers at run time, and
 * sdc.h says the memory macros may move between minor releases. So the build
 * time number can be right and the run time one larger. Reporting both means a
 * host can see the headroom rather than only finding out when a controller
 * refuses to enable, and can watch it shrink across an nrfxlib upgrade.
 *
 * Both read zero on a stack whose platform layer never filled them in, which
 * is what the host tests do, so zero means not reported rather than no memory.
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
 *  32  SdcMemRequired                   octets sdc_cfg_set asked for
 *  33  SdcMemCapacity                   octets the build reserved
 *  34  PawrDelayedCandidateCount        structurally valid 0x2083 commands seen
 *  35  PawrDelayedHandlerCallCount      0x2083 commands that entered the handler
 *  36  PawrSyntheticSuppressedCount     local 0x2083 completions suppressed
 *  37  PawrSdcCompleteCount             matching real SDC completions received
 *  38  ControllerAclPacketCount         valid ACL packets fetched from controller
 *  39  HostAclPacketCount               ACL packets accepted by the host transport
 *  40  UsbEventEpDataCount              EP1 host transactions consumed
 *  41  UsbEventDmaEndCount              EP1 RAM-to-endpoint DMA completions
 *  42  UsbEventContinueCount            EP1 transactions that started another chunk
 *  43  UsbEventCompleteCount            EP1 logical transfers completed
 *  44  UsbEventBadAmountCount           EP1 AMOUNT differed from requested chunk
 *  45  UsbEventStaleStatusCount         EP1 status already set when a new Event armed
 *  46  UsbEventEp2CollisionCount        EP1 and EPOUT2 status were pending together
 *  47  UsbEventLateStatusCount          EP1 appeared before a shared EPDATA clear
 *  48  UsbEventEndOverlapCount          EP1 END and direct EPOUT2 END shared one IRQ
 *  49-61 UsbEventAckTrace               last thirteen ACKed EP1 payload chunks
 *
 * Each UsbEventAckTrace word stores the first three bytes of the chunk in bits
 * 0..23 and the zero-based chunk index within that logical Event transfer in
 * bits 24..31. The thirteen words are returned oldest to newest. They are a
 * diagnostic trace, not monotonically increasing counters.
 */
#define HCI_COUNTERS_VERSION    8U
#define HCI_COUNTERS_COUNT      62U
#define HCI_COUNTERS_RETURN_LEN (1U + (HCI_COUNTERS_COUNT * 4U))

/* First index reserved for target/platform diagnostic counters. */
#define HCI_COUNTERS_PLATFORM_FIRST 40U
#define HCI_COUNTERS_PLATFORM_COUNT \
    (HCI_COUNTERS_COUNT - HCI_COUNTERS_PLATFORM_FIRST)

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

    /*
     * Filled in by the platform layer once the controller has answered, and
     * left at zero everywhere else. Plain integers rather than a pointer to
     * the target structure, because this layer is portable and that one is
     * not: the dispatch test builds this file with no nRF52840 in sight.
     */
    uint32_t SdcMemRequired;
    uint32_t SdcMemCapacity;
} HciCounters_t;

void HciCountersInit(HciCounters_t *pCounters,
                     HciSdc_t *pSdc,
                     const HciController_t *pController);

/*
 * The pool figures, reported at indices 32 and 33. Separate from the init
 * above because they are not known until sdc_cfg_set has answered, which
 * happens after the counter layer has to be wired.
 */
void HciCountersSetSdcMem(HciCounters_t *pCounters,
                          uint32_t Required,
                          uint32_t Capacity);

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

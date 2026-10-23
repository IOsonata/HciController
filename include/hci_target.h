/**-------------------------------------------------------------------------
@file	hci_target.h

@brief	HciController target, the MCU port a project links.

		Declares what each target file defines: initialization, start
		and stop of MPSL and the SDC, MPSL servicing, UART diagnostics,
		SDC memory reporting and target error access.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_TARGET_H
#define HCI_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_taktos.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The MCU port of HciController. Each target file defines these for its MCU
 * (hci_nrf52840.cpp, hci_nrf5340_net.cpp, hci_nrf54lm20.cpp) and a project
 * links the one of its MCU. Hardware initialization, MPSL integration and
 * interrupt ownership belong to the target; the SDC feature configuration is
 * shared, in hci_sdc_resources.h.
 */

/*
 * Keep what the target needs to start later: the runtime its interrupts wake
 * and the SDC memory pool, 8 byte aligned. Touches no hardware.
 */
bool HciTargetInit(HciTaktOs_t *pRuntime, uint8_t *pSdcMem, size_t SdcMemCapacity);

/* Bring up MPSL, then the SoftDevice Controller. Runs on the runtime thread. */
bool HciTargetStart(void);

/* MPSL low priority work, run by the runtime thread on HCI_TAKTOS_EVENT_MPSL. */
void HciTargetProcessMpsl(void);

/* Record a runtime start failure. */
void HciTargetFault(int Error);

/* Take down the SoftDevice Controller, then MPSL, and their interrupts. */
void HciTargetStop(void);

/* Required and allocated SDC memory in bytes, 0 when not known yet. */
void HciTargetGetSdcMem(uint32_t *pRequired, uint32_t *pCapacity);

/* The most recent target error. */
int32_t HciTargetLastError(void);

/*
 * UART host link diagnostics: peripheral state, pin mapping, error status and
 * hardware flow control signals. hci_app.cpp has a weak default that prints
 * nothing, for a target that does not decode its UART.
 */
void HciTargetUartTrace(uint8_t DevNo);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TARGET_H */

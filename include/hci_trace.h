/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef HCI_TRACE_H
#define HCI_TRACE_H

#include <stdint.h>

struct __device_intrf;
typedef struct __device_intrf DevIntrf_t;

#ifndef HCI_TRACE
#define HCI_TRACE 0
#endif

#define HCI_TRACE_RECORD_SIZE  160U
#define HCI_TRACE_RECORD_COUNT 26U

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Developer trace. Every call is queued in the standard IOsonata SysLog
 * CFifo. HCI_TRACE only enables the optional ARM semihosting copy.
 */
void HciTrace(const char *pFormat, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void HciTraceInit(void);

/* Select or detach the DeviceIntrf used when queued records are flushed. */
void HciTraceSetSink(DevIntrf_t *pSink, uint32_t SinkAddr);

/* Flush one complete SysLog record through the selected DeviceIntrf. */
int HciTraceFlush(void);

/* Number of complete records currently queued in the SysLog CFifo. */
uint32_t HciTracePending(void);

/* Number of old records pushed out because the SysLog CFifo was full. */
uint32_t HciTraceDropped(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TRACE_H */

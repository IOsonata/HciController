/**-------------------------------------------------------------------------
@file	hci_trace.h

@brief	HciController diagnostic trace interface.

		Declares the buffered SysLog trace API, DeviceIntrf sink selection,
		and flush operation.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_TRACE_H
#define HCI_TRACE_H

#include <stdint.h>

struct __device_intrf;
typedef struct __device_intrf DevIntrf_t;

#ifndef HCI_TRACE
#define HCI_TRACE 0
#endif

#ifndef HCI_TRACE_RECORD_SIZE
#define HCI_TRACE_RECORD_SIZE  160U
#endif
#ifndef HCI_TRACE_RECORD_COUNT
#define HCI_TRACE_RECORD_COUNT 26U
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Developer trace. Every call is written through the standard IOsonata
 * SysLog. HCI_TRACE only enables the optional ARM semihosting copy.
 */
void HciTrace(const char *pFormat, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Error report. Same output as HciTrace, but it is pushed out and waited for
 * (bounded) before it returns, so the text is on the log when the failure
 * happens and not left in the store by a stop or reset that follows. */
void HciTraceError(const char *pFormat, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void HciTraceInit(void);

/* Select or detach the DeviceIntrf used by SysLog output. */
void HciTraceSetSink(DevIntrf_t *pSink, uint32_t SinkAddr);

/* Flush queued SysLog records through the selected DeviceIntrf. */
int HciTraceFlush(void);

/* While set, every record is pushed out and waited for (bounded) before
 * HciTrace returns. For start up only, before the scheduler runs. */
void HciTraceBootDrain(bool bEnable);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TRACE_H */

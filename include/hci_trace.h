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

#define HCI_TRACE_RECORD_SIZE  160U
#define HCI_TRACE_RECORD_COUNT 26U

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

void HciTraceInit(void);

/* Select or detach the DeviceIntrf used by SysLog output. */
void HciTraceSetSink(DevIntrf_t *pSink, uint32_t SinkAddr);

/* Flush queued SysLog records through the selected DeviceIntrf. */
int HciTraceFlush(void);

#ifdef __cplusplus
}
#endif

#endif /* HCI_TRACE_H */

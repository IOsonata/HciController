/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * Bring up trace over semihosting. Build with HCI_TRACE=1.
 *
 * Output goes out through the SYS_WRITE0 semihosting call, so nothing has to
 * be linked in beyond vsnprintf. No rdimon specs, no syscall stubs, no change
 * to the link line, which keeps this from colliding with the IOsonata and
 * TaktOS libraries.
 *
 * A debugger with semihosting enabled must be attached. Without one the BKPT
 * is an undefined instruction and takes the HardFault handler, so this must
 * stay off in anything that ships.
 *   SEGGER J-Link  : monitor semihosting enable
 *   OpenOCD        : arm semihosting enable
 *   pyOCD          : on by default
 *
 * Every call halts the core for the duration of the transfer, so trace points
 * belong in start up paths only, never in the packet path.
 */

#ifndef HCI_TRACE_H
#define HCI_TRACE_H

/*
 * Off unless the build asks for it. The Debug and Debug_MBR configurations in
 * IOcomposer/.cproject define HCI_TRACE=1, and the make build takes it on the
 * command line or in local.mk.
 */
#ifndef HCI_TRACE
#define HCI_TRACE 0
#endif

#if HCI_TRACE

#include <stdarg.h>
#include <stdio.h>

#ifndef HCI_TRACE_LINE_SIZE
#define HCI_TRACE_LINE_SIZE 160
#endif

#define HCI_TRACE_SYS_WRITE0 0x04

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((unused))
static void HciTraceWrite0(const char *pStr)
{
    register const char *arg __asm__("r1") = pStr;
    register int op __asm__("r0") = HCI_TRACE_SYS_WRITE0;

    __asm__ volatile ("bkpt 0xAB"
                      : "+r" (op)
                      : "r" (arg)
                      : "memory");
    (void)op;
}

__attribute__((format(printf, 1, 2), unused))
static void HciTrace(const char *pFmt, ...)
{
    char line[HCI_TRACE_LINE_SIZE];
    va_list args;

    va_start(args, pFmt);
    (void)vsnprintf(line, sizeof(line), pFmt, args);
    va_end(args);

    HciTraceWrite0(line);
}

__attribute__((unused))
static void HciTraceInit(void)
{
    HciTraceWrite0("\r\n--- HciController trace ---\r\n");
}

#ifdef __cplusplus
}
#endif

#else

#define HciTraceInit() do { } while (0)
#define HciTrace(...)  do { } while (0)

#endif

#endif /* HCI_TRACE_H */

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
 * Bring up trace. It goes to two places and they are reached differently.
 *
 * The log always gets it. hci_syslog.cpp holds a ring that is a valid empty
 * ring the moment BSS is cleared, so a trace call works before anything has
 * been initialised and needs no start up of its own. That is the copy a
 * person can read on a board with no debugger on it, which is every board
 * once it leaves a bench.
 *
 * Semihosting only when HCI_TRACE=1. Output goes through the SYS_WRITE0 call,
 * so nothing has to be linked in beyond vsnprintf: no rdimon specs, no
 * syscall stubs, no change to the link line, which keeps this from colliding
 * with the IOsonata and TaktOS libraries. A debugger with semihosting enabled
 * must be attached. Without one the BKPT is an undefined instruction and takes
 * the HardFault handler, so this half must stay off in anything that ships.
 *   SEGGER J-Link  : monitor semihosting enable
 *   OpenOCD        : arm semihosting enable
 *   pyOCD          : on by default
 *
 * The two used to be one, both behind HCI_TRACE, and that made the log
 * unreachable: with tracing off nothing was written to it, and with tracing on
 * a board without a debugger faulted on the first call. So every build had an
 * empty log port and no build could have filled it.
 *
 * A semihosting call halts the core for the duration of the transfer and a log
 * call formats a line, so trace points belong in start up paths and the
 * runtime thread, never in the packet path and never in an interrupt: the ring
 * has one writer and no lock.
 */

#ifndef HCI_TRACE_H
#define HCI_TRACE_H

#include <stdarg.h>
#include <stdio.h>

/*
 * The semihosting half, off unless the build asks for it. The log half does
 * not read this and is always there.
 */
#ifndef HCI_TRACE
#define HCI_TRACE 0
#endif

#ifndef HCI_TRACE_LINE_SIZE
#define HCI_TRACE_LINE_SIZE 160
#endif

#define HCI_TRACE_SYS_WRITE0 0x04

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the line goes. Declared rather than included, so this header keeps no
 * dependency on hci_syslog.h and every file that traces does not acquire one.
 */
void HciSyslogTraceLine(const char *pLine);

#if HCI_TRACE

__attribute__((unused))
static void HciTraceWrite0(const char *pStr)
{
#if defined(__arm__)
    register const char *arg __asm__("r1") = pStr;
    register int op __asm__("r0") = HCI_TRACE_SYS_WRITE0;

    __asm__ volatile ("bkpt 0xAB"
                      : "+r" (op)
                      : "r" (arg)
                      : "memory");
    (void)op;
#else
    /*
     * Semihosting is an ARM debug call, so a host build has to go somewhere
     * else. This is not there to be useful on a workstation. It is there so a
     * host build can compile with HCI_TRACE=1, which is the only way the
     * arguments to every trace call get checked against their format string:
     * with tracing off the macro discards them and a wrong one is invisible
     * until an ARM build with tracing on.
     */
    fputs(pStr, stderr);
#endif
}

#else

/*
 * Nothing, and not a macro, so the line still reaches the log below and the
 * call site is the same shape either way.
 */
__attribute__((unused))
static void HciTraceWrite0(const char *pStr)
{
    (void)pStr;
}

#endif

/*
 * Formatted once and handed to both. A bench with a debugger attached does not
 * lose the trace it has always had, and a sealed board still gets the line.
 */
__attribute__((format(printf, 1, 2), unused))
static void HciTrace(const char *pFmt, ...)
{
    char line[HCI_TRACE_LINE_SIZE];
    va_list args;

    va_start(args, pFmt);
    const int len = vsnprintf(line, sizeof(line), pFmt, args);
    va_end(args);

    /*
     * Say when a line did not fit, rather than let it end wherever the buffer
     * did. A number cut in half reads as a smaller number and not as a
     * missing one, which is how a worst case of sixty thousand cycles was
     * read off this log as six, twice, before anyone counted the characters.
     */
    if (len >= (int)sizeof(line))
    {
        line[sizeof(line) - 4U] = '.';
        line[sizeof(line) - 3U] = '.';
        line[sizeof(line) - 2U] = '.';
        line[sizeof(line) - 1U] = '\0';
    }

    HciTraceWrite0(line);
    HciSyslogTraceLine(line);
}

__attribute__((unused))
static void HciTraceInit(void)
{
    HciTrace("\r\n--- HciController trace ---\r\n");
}

#ifdef __cplusplus
}
#endif

#endif /* HCI_TRACE_H */

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
 * A log the board can send somewhere a person is.
 *
 * Trace goes out over semihosting, which needs a debugger attached and
 * reaches nobody without one. That is fine on a bench and useless on a sealed
 * dongle, on a customer's board, or on somebody else's product where the only
 * thing that can be observed is what comes out of a port.
 *
 * This is the other half: a ring the firmware writes into and a port drains.
 * Three properties matter and they are all about not making the fault worse.
 *
 * It never blocks. A full ring drops the oldest line and counts the drop. A
 * log that stalls the thread it is logging from turns an observation into a
 * second fault, and the first thing anyone would log is a path that is
 * already going wrong.
 *
 * It never fails the caller. There is no error to check and nothing to
 * handle, so a log call cannot grow a branch that itself needs logging.
 *
 * It drains from the same thread as everything else. Nothing here touches a
 * port from an interrupt, so the ordering between a log line and the work it
 * describes is the ordering the thread saw.
 *
 * What it is not: it is not the HCI stream, and nothing on it is parsed by
 * anything. A host that opens the port gets text.
 */

#ifndef HCI_SYSLOG_H
#define HCI_SYSLOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sized for a fault that arrives in a burst rather than for steady output.
 * The interesting case is twenty lines in a millisecond while something goes
 * wrong, and then silence, so the ring is what decides whether the beginning
 * of that burst survives to be read.
 */
#ifndef HCI_SYSLOG_SIZE
#define HCI_SYSLOG_SIZE 2048U
#endif

/*
 * One line's worth of formatting scratch. A line longer than this is
 * truncated rather than split, since half a line with a known end is easier
 * to read than a line that may or may not have been cut.
 */
#ifndef HCI_SYSLOG_LINE_MAX
#define HCI_SYSLOG_LINE_MAX 160U
#endif

typedef struct {
    uint8_t Buffer[HCI_SYSLOG_SIZE];

    /* Where the next octet is written and where the next one is read. */
    size_t Head;
    size_t Tail;

    /*
     * Octets dropped because the ring was full, and lines truncated because
     * they were longer than the scratch. Both are reported in the log itself
     * when there is room again, because a log that silently loses the part
     * that mattered is worse than one that says it did.
     */
    uint32_t DroppedOctets;
    uint32_t TruncatedLines;

    /* Set once a drop has happened and cleared when it has been reported. */
    bool DropPending;
} HciSyslog_t;

/*
 * The port a drain writes to. Returns how many octets it took, which may be
 * fewer than offered and may be zero when nothing is listening. Whatever it
 * does not take stays in the ring for the next drain.
 */
typedef size_t (*HciSyslogWrite_t)(void *pContext, const uint8_t *pData,
                                   size_t Len);

void HciSyslogInit(HciSyslog_t *pLog);

/*
 * Write one line. A newline is added if the format did not end with one, so
 * every line in the ring ends the same way and a reader never has to guess
 * whether the tail is a partial line or a line without an ending.
 */
void HciSyslogPrint(HciSyslog_t *pLog, const char *pFormat, ...)
    __attribute__((format(printf, 2, 3)));

void HciSyslogPrintV(HciSyslog_t *pLog, const char *pFormat, va_list Args);

/*
 * Hand what is queued to the port. Called from the runtime thread, as often
 * as it likes; nothing here needs a timer or a rate.
 */
void HciSyslogDrain(HciSyslog_t *pLog, HciSyslogWrite_t Write, void *pContext);

/* What is waiting, for a caller that wants to know before draining. */
size_t HciSyslogPending(const HciSyslog_t *pLog);

#ifdef __cplusplus
}
#endif

#endif /* HCI_SYSLOG_H */

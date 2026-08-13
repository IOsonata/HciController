/*
 * Copyright (c) 2026 I-SYST inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "hci_syslog.h"

#include <stdio.h>
#include <string.h>

/*
 * One free octet is kept between head and tail so the two never meet on a
 * full ring. Without it, full and empty are the same pair of indices and the
 * only way to tell them apart is a third field that has to be right at every
 * point two of them are updated.
 */
static size_t HciSyslogUsed(const HciSyslog_t *pLog)
{
    return (pLog->Head + HCI_SYSLOG_SIZE - pLog->Tail) % HCI_SYSLOG_SIZE;
}

static size_t HciSyslogFree(const HciSyslog_t *pLog)
{
    return HCI_SYSLOG_SIZE - 1U - HciSyslogUsed(pLog);
}

/*
 * Make room by giving up whole lines from the oldest end.
 *
 * Dropping octets would leave the reader a line that starts in the middle of
 * a sentence and no way to know it. Dropping to the next newline costs a
 * little more of the log and leaves every line in the ring a whole one.
 */
static void HciSyslogDropOldest(HciSyslog_t *pLog, size_t Needed)
{
    /*
     * The tail always sits at the start of a line, so the loop has two
     * reasons to keep going and needs both: there is not enough room yet, or
     * there is but it stopped part way through a line.
     *
     * Only the first of those was here at first, and the ring then handed a
     * reader a line beginning in the middle of a word with nothing to say it
     * had. The test caught it, which is the case it was written for.
     */
    bool atLineStart = true;

    while (pLog->Tail != pLog->Head &&
           (HciSyslogFree(pLog) < Needed || !atLineStart))
    {
        const uint8_t octet = pLog->Buffer[pLog->Tail];
        pLog->Tail = (pLog->Tail + 1U) % HCI_SYSLOG_SIZE;
        pLog->DroppedOctets++;
        pLog->DropPending = true;
        atLineStart = (octet == (uint8_t)'\n');
    }
}

static void HciSyslogPut(HciSyslog_t *pLog, const char *pData, size_t Len)
{
    if (Len == 0U)
    {
        return;
    }

    /*
     * A line longer than the whole ring cannot be held at all. Keeping the
     * end of it rather than the start would leave the reader the least useful
     * part, so the ring keeps what fits from the start.
     */
    if (Len > HCI_SYSLOG_SIZE - 1U)
    {
        Len = HCI_SYSLOG_SIZE - 1U;
    }

    HciSyslogDropOldest(pLog, Len);

    for (size_t i = 0U; i < Len; i++)
    {
        pLog->Buffer[pLog->Head] = (uint8_t)pData[i];
        pLog->Head = (pLog->Head + 1U) % HCI_SYSLOG_SIZE;
    }
}

/*
 * The one log the image has.
 *
 * It lives here rather than inside the application structure, and that is the
 * point of it. Everything in BSS starts zeroed, and a zeroed ring is a valid
 * empty ring, so this one works from the first instruction that runs. Nothing
 * has to initialise it and nothing can be too early to write to it, which
 * matters because the lines worth having are the ones from a start up that did
 * not finish. Held inside the application it was cleared by that structure's
 * own memset, so every line written before then was lost, and a failure to
 * initialise at all left nothing to read.
 */
static HciSyslog_t s_DefaultLog;

HciSyslog_t *HciSyslogDefault(void)
{
    return &s_DefaultLog;
}

/*
 * The log trace goes to. A single pointer rather than an argument on every
 * call, because trace is written from files that have no reason to know a log
 * exists and every reason not to take one as a parameter. Pointed at the
 * default from the start, so tracing needs no attaching either.
 */
static HciSyslog_t *s_pTraceLog = &s_DefaultLog;

void HciSyslogAttachTrace(HciSyslog_t *pLog)
{
    s_pTraceLog = pLog;
}

extern "C" void HciSyslogTraceLine(const char *pLine)
{
    if (s_pTraceLog == NULL || pLine == NULL)
    {
        return;
    }

    HciSyslogWrite(s_pTraceLog, pLine);
}

void HciSyslogWrite(HciSyslog_t *pLog, const char *pText)
{
    if (pLog == NULL || pText == NULL)
    {
        return;
    }

    /*
     * Straight in as text. Going through the formatter with "%s" would read a
     * percent sign in the line as a format and take arguments that are not
     * there, and would spend a second 160 octet frame on a line that has
     * already been built in one. Trace calls this from a thread with a stack
     * to keep, and HciTrace already holds a buffer that size.
     */
    size_t len = strlen(pText);
    HciSyslogPut(pLog, pText, len);

    if (len > HCI_SYSLOG_SIZE - 1U)
    {
        len = HCI_SYSLOG_SIZE - 1U;
    }

    if (len == 0U || pText[len - 1U] != '\n')
    {
        HciSyslogPut(pLog, "\n", 1U);
    }
}

void HciSyslogInit(HciSyslog_t *pLog)
{
    if (pLog == NULL)
    {
        return;
    }

    memset(pLog, 0, sizeof(*pLog));
}

void HciSyslogPrintV(HciSyslog_t *pLog, const char *pFormat, va_list Args)
{
    if (pLog == NULL || pFormat == NULL)
    {
        return;
    }

    char line[HCI_SYSLOG_LINE_MAX];
    const int written = vsnprintf(line, sizeof(line), pFormat, Args);
    if (written < 0)
    {
        return;
    }

    size_t len = (size_t)written;
    if (len >= sizeof(line))
    {
        /* vsnprintf reports what it would have written, not what it did. */
        len = sizeof(line) - 1U;
        pLog->TruncatedLines++;
    }

    HciSyslogPut(pLog, line, len);

    /*
     * Every line ends the same way, so a reader never has to decide whether
     * the tail of the ring is a partial line or a line without an ending.
     */
    if (len == 0U || line[len - 1U] != '\n')
    {
        HciSyslogPut(pLog, "\n", 1U);
    }
}

void HciSyslogPrint(HciSyslog_t *pLog, const char *pFormat, ...)
{
    va_list args;
    va_start(args, pFormat);
    HciSyslogPrintV(pLog, pFormat, args);
    va_end(args);
}

size_t HciSyslogPending(const HciSyslog_t *pLog)
{
    return pLog == NULL ? 0U : HciSyslogUsed(pLog);
}

void HciSyslogDrain(HciSyslog_t *pLog, HciSyslogWrite_t Write, void *pContext)
{
    if (pLog == NULL || Write == NULL)
    {
        return;
    }

    /*
     * Say what was lost, before the lines that survived it.
     *
     * Written straight to the port rather than into the ring. The ring is
     * full at exactly the moment this needs saying, so a note that competes
     * for room in it is a note that never appears in the one case it exists
     * for. It went in that way first and the test caught it.
     *
     * Kept pending unless the port takes the whole note, so a port that took
     * half of it does not leave a reader a sentence that stops.
     */
    if (pLog->DropPending)
    {
        char note[64];
        const int len = snprintf(note, sizeof(note),
                                 "syslog: %lu octet(s) dropped, log full\n",
                                 (unsigned long)pLog->DroppedOctets);
        if (len > 0 && (size_t)len < sizeof(note))
        {
            const size_t taken = Write(pContext, (const uint8_t *)note,
                                       (size_t)len);
            if (taken == (size_t)len)
            {
                pLog->DropPending = false;
            }
        }
    }

    while (pLog->Tail != pLog->Head)
    {
        /*
         * One contiguous run at a time, so the port is handed a plain pointer
         * and never has to know the buffer wraps.
         */
        size_t run = (pLog->Head > pLog->Tail) ?
                     (pLog->Head - pLog->Tail) :
                     (HCI_SYSLOG_SIZE - pLog->Tail);

        const size_t taken = Write(pContext, &pLog->Buffer[pLog->Tail], run);
        if (taken == 0U)
        {
            /* Nothing is listening. What is queued stays queued. */
            return;
        }

        pLog->Tail = (pLog->Tail + taken) % HCI_SYSLOG_SIZE;

        if (taken < run)
        {
            /* The port is full for now. The rest waits for the next drain. */
            return;
        }
    }
}

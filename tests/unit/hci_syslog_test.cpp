/*
 * The log ring.
 *
 * The properties worth pinning are the ones that only matter when something
 * is already going wrong: that a full ring loses whole lines rather than
 * halves of them, that it says it lost them, that a port taking nothing or
 * taking part of what it is offered leaves the rest queued rather than
 * dropping it, and that none of it blocks or fails the caller.
 *
 * A log is only useful in the case it is hardest to test by hand, so it is
 * tested here instead.
 */

#include "hci_syslog.h"
#include "hci_trace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static HciSyslog_t gLog;

/* A port that takes everything, and keeps it for inspection. */
static char gSink[HCI_SYSLOG_SIZE * 4];
static size_t gSinkLen;

static size_t SinkAll(void *, const uint8_t *pData, size_t Len)
{
    assert(gSinkLen + Len < sizeof(gSink));
    memcpy(&gSink[gSinkLen], pData, Len);
    gSinkLen += Len;
    gSink[gSinkLen] = '\0';
    return Len;
}

/* A port that is not there. */
static size_t SinkNone(void *, const uint8_t *, size_t)
{
    return 0U;
}

/* A port that takes one octet per call, which is the worst legal case. */
static size_t SinkOne(void *, const uint8_t *pData, size_t Len)
{
    if (Len == 0U)
    {
        return 0U;
    }
    gSink[gSinkLen++] = (char)pData[0];
    gSink[gSinkLen] = '\0';
    return 1U;
}

static void Reset(void)
{
    HciSyslogInit(&gLog);
    gSinkLen = 0U;
    gSink[0] = '\0';
}

static size_t CountLines(const char *pText)
{
    size_t lines = 0U;
    for (const char *p = pText; *p != '\0'; ++p)
    {
        if (*p == '\n')
        {
            lines++;
        }
    }
    return lines;
}

int main(void)
{
    /* A line comes back whole, with the ending added. */
    {
        Reset();
        HciSyslogPrint(&gLog, "boot %s %d", "uart", 7);
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(strcmp(gSink, "boot uart 7\n") == 0);
        printf("[ok] %-42s %s", "a line comes back whole", gSink);
    }

    /* An ending already there is not doubled. */
    {
        Reset();
        HciSyslogPrint(&gLog, "already ended\n");
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(strcmp(gSink, "already ended\n") == 0);
        printf("[ok] %-42s one ending, not two\n", "an ending is not doubled");
    }

    /* Nothing listening leaves everything queued. */
    {
        Reset();
        HciSyslogPrint(&gLog, "held");
        const size_t pending = HciSyslogPending(&gLog);
        assert(pending == 5U);
        HciSyslogDrain(&gLog, SinkNone, NULL);
        assert(HciSyslogPending(&gLog) == pending);
        assert(gSinkLen == 0U);
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(strcmp(gSink, "held\n") == 0);
        printf("[ok] %-42s %zu octets kept, then sent\n",
               "a port that is not there loses nothing", pending);
    }

    /* A port taking one octet at a time still delivers everything in order. */
    {
        Reset();
        for (int i = 0; i < 5; i++)
        {
            HciSyslogPrint(&gLog, "line %d", i);
        }
        for (int i = 0; i < 200; i++)
        {
            HciSyslogDrain(&gLog, SinkOne, NULL);
        }
        assert(strcmp(gSink, "line 0\nline 1\nline 2\nline 3\nline 4\n") == 0);
        assert(HciSyslogPending(&gLog) == 0U);
        printf("[ok] %-42s five lines, in order\n",
               "one octet a call still drains");
    }

    /*
     * A full ring drops from the oldest end and what is left is whole lines.
     * This is the case the whole thing exists for: a burst during a fault,
     * where the question is whether what survives can be read.
     */
    {
        Reset();
        for (int i = 0; i < 400; i++)
        {
            HciSyslogPrint(&gLog, "a line that is long enough to matter %d", i);
        }
        HciSyslogDrain(&gLog, SinkAll, NULL);

        assert(gLog.DroppedOctets > 0U);

        /* Everything delivered ends in a newline, so no half lines survived. */
        assert(gSinkLen > 0U);
        assert(gSink[gSinkLen - 1U] == '\n');

        /* And every line that is there starts where a line should. */
        const char *p = gSink;
        size_t whole = 0U;
        while (*p != '\0')
        {
            const char *end = strchr(p, '\n');
            assert(end != NULL);
            assert(strncmp(p, "a line that is long enough", 26) == 0 ||
                   strncmp(p, "syslog:", 7) == 0);
            whole++;
            p = end + 1;
        }
        assert(whole == CountLines(gSink));
        printf("[ok] %-42s %zu whole lines, %lu octets dropped\n",
               "a full ring loses whole lines", whole,
               (unsigned long)gLog.DroppedOctets);
    }

    /* And it says that it dropped them. */
    {
        Reset();
        for (int i = 0; i < 400; i++)
        {
            HciSyslogPrint(&gLog, "filling the ring right up %d", i);
        }
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(strstr(gSink, "dropped") != NULL);
        printf("[ok] %-42s the log says so itself\n", "a drop is reported");
    }

    /* A line past the scratch is truncated, counted, and still ends. */
    {
        Reset();
        char huge[HCI_SYSLOG_LINE_MAX * 2];
        memset(huge, 'x', sizeof(huge) - 1U);
        huge[sizeof(huge) - 1U] = '\0';
        HciSyslogPrint(&gLog, "%s", huge);
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(gLog.TruncatedLines == 1U);
        assert(gSinkLen == HCI_SYSLOG_LINE_MAX);
        assert(gSink[gSinkLen - 1U] == '\n');
        printf("[ok] %-42s %zu octets, counted, still ended\n",
               "an over long line is truncated", gSinkLen);
    }

    /*
     * The default log takes lines with nothing called first, because it is in
     * BSS and a zeroed ring is an empty one. A start up that fails before it
     * initialises anything still has somewhere to have said why.
     */
    {
        gSinkLen = 0U;
        gSink[0] = '\0';
        HciSyslogPrint(HciSyslogDefault(), "before anything");
        HciSyslogDrain(HciSyslogDefault(), SinkAll, NULL);
        assert(strcmp(gSink, "before anything\n") == 0);
        printf("[ok] %-42s no init needed\n",
               "the default log is ready at reset");
    }

    /*
     * And trace reaches it, whatever HCI_TRACE says.
     *
     * This is the case the whole port exists for and the one that was broken:
     * the log was fed only from inside HciTrace, and HciTrace was compiled
     * away unless the build asked for semihosting. Every ordinary build
     * therefore had a log port that enumerated, opened, and stayed empty
     * forever, and the only build that would have filled it faults on the
     * first line without a debugger attached. Nothing tested the path from a
     * trace call to the ring, so nothing said so.
     */
    {
        gSinkLen = 0U;
        gSink[0] = '\0';
        HciTrace("boot: host=%s\r\n", "uart");
        HciSyslogDrain(HciSyslogDefault(), SinkAll, NULL);
        assert(strstr(gSink, "boot: host=uart") != NULL);
        printf("[ok] %-42s trace=%d and the line still arrives\n",
               "trace reaches the log", HCI_TRACE);
    }

    /*
     * A line already built goes in as text. The percent sign is the case that
     * matters: through the formatter it would be read as a conversion and take
     * an argument that is not there, and trace lines are built by callers that
     * had no idea their text would be formatted twice.
     */
    {
        Reset();
        HciSyslogWrite(&gLog, "usb: 50% duty, eventcause=0x%08lX seen");
        HciSyslogDrain(&gLog, SinkAll, NULL);
        assert(strcmp(gSink, "usb: 50% duty, eventcause=0x%08lX seen\n") == 0);
        printf("[ok] %-42s a percent sign is a percent sign\n",
               "a built line is not formatted again");
    }

    /* A null log and a null port are not a caller's problem. */
    {
        Reset();
        HciSyslogPrint(NULL, "nowhere");
        HciSyslogDrain(NULL, SinkAll, NULL);
        HciSyslogDrain(&gLog, NULL, NULL);
        assert(HciSyslogPending(NULL) == 0U);
        printf("[ok] %-42s nothing to check, nothing to handle\n",
               "a log call cannot fail");
    }

    printf("\nAll syslog tests passed.\n");
    return 0;
}

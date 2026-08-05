/*
 * Host test for the nrf_sw_lpuart handshake.
 *
 * The sequence matters more than any single step: the peer reads a level on
 * the wire rather than an edge, and it starts sending the instant the wire
 * falls. So the order in which the pin is driven, released and watched, and
 * where the receiver is enabled relative to the acknowledge, is what decides
 * whether the two sides talk at all. Every ordering below is asserted rather
 * than assumed, because none of it can be seen from the outside once it is
 * running on a board.
 */

#include "hci_lpuart.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

/* Everything the state machine did, in order, as readable text. */
static std::vector<std::string> gLog;
static uint32_t gNowMs;
static bool gSendOk = true;

static void LogAdd(const char *pText)
{
    gLog.push_back(pText);
}

static void ReqDrive(void *, bool High)
{
    LogAdd(High ? "req=1" : "req=0");
}

static void ReqRelease(void *)
{
    LogAdd("req=release");
}

static void RdyDrive(void *, bool High)
{
    LogAdd(High ? "rdy=1" : "rdy=0");
}

static void RdyRelease(void *)
{
    LogAdd("rdy=release");
}

static void RdyWatch(void *, bool Level)
{
    LogAdd(Level ? "rdy=watch-level" : "rdy=watch-fall");
}

static bool Send(void *, const uint8_t *, size_t Len)
{
    char text[32];
    snprintf(text, sizeof(text), "send=%zu", Len);
    LogAdd(text);
    return gSendOk;
}

static void RxEnable(void *, bool Enable)
{
    LogAdd(Enable ? "rx=on" : "rx=off");
}

static uint32_t Ms(void *)
{
    return gNowMs;
}

static HciLpUartOps_t MakeOps(void)
{
    HciLpUartOps_t ops = {};
    ops.ReqDrive = ReqDrive;
    ops.ReqRelease = ReqRelease;
    ops.RdyDrive = RdyDrive;
    ops.RdyRelease = RdyRelease;
    ops.RdyWatch = RdyWatch;
    ops.Send = Send;
    ops.RxEnable = RxEnable;
    ops.Ms = Ms;
    ops.AckTimeoutMs = 10U;
    return ops;
}

static void Reset(HciLpUart_t *pLp)
{
    gLog.clear();
    gNowMs = 0U;
    gSendOk = true;
    HciLpUartOps_t ops = MakeOps();
    assert(HciLpUartInit(pLp, &ops));
    gLog.clear();
}

static std::string Joined(void)
{
    std::string out;
    for (size_t i = 0U; i < gLog.size(); i++)
    {
        if (i > 0U)
        {
            out += " ";
        }
        out += gLog[i];
    }
    return out;
}

static void Expect(const char *pLabel, const char *pExpected)
{
    const std::string got = Joined();
    if (got != pExpected)
    {
        printf("\n%s\n  expected: %s\n  got:      %s\n",
               pLabel, pExpected, got.c_str());
        assert(false);
    }
    printf("[ok] %-44s %s\n", pLabel, pExpected);
    gLog.clear();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    static HciLpUart_t lp;
    static const uint8_t payload[] = {0x01U, 0x03U, 0x0CU, 0x00U};

    /*
     * Bring up. Both wires rest and only the level that starts an incoming
     * transfer is watched, so a peer already running does not read a request
     * out of a pin this side has not driven yet.
     */
    {
        gLog.clear();
        gNowMs = 0U;
        HciLpUartOps_t ops = MakeOps();
        assert(HciLpUartInit(&lp, &ops));
        Expect("init rests both wires",
               "req=0 rx=off rdy=release rdy=watch-level");
    }

    /* An unusable ops table is refused rather than half accepted. */
    {
        HciLpUart_t bad;
        HciLpUartOps_t ops = MakeOps();
        ops.Send = NULL;
        assert(!HciLpUartInit(&bad, &ops));
        ops = MakeOps();
        ops.RdyWatch = NULL;
        assert(!HciLpUartInit(&bad, &ops));
        assert(!HciLpUartInit(NULL, &ops));
        printf("[ok] %-44s\n", "init refuses an incomplete ops table");
    }

    /*
     * Sending. Drive the request, then let go, so the pull up holds the level
     * while the peer answers. Nothing goes out until it has.
     */
    Reset(&lp);
    assert(HciLpUartSend(&lp, payload, sizeof(payload)));
    Expect("a request drives then releases the wire", "req=1 req=release");
    assert(HciLpUartSendBusy(&lp));

    /* A second send while one is in flight is refused, not queued. */
    assert(!HciLpUartSend(&lp, payload, sizeof(payload)));
    assert(gLog.empty());
    printf("[ok] %-44s\n", "a second send is refused while one waits");

    /* The peer pulls the wire low. Only now do the bytes go out. */
    HciLpUartReqFell(&lp);
    Expect("the acknowledge drives high and sends", "req=1 send=4");

    /* And the wire falls at the end, which is what stops the peer receiving. */
    HciLpUartSendDone(&lp);
    Expect("the end of the bytes releases the wire", "req=0");
    assert(!HciLpUartSendBusy(&lp));

    /*
     * Receiving. The receiver has to be ready before the wire is touched: the
     * peer starts sending on the fall, so acknowledging first would lose the
     * leading bytes.
     */
    Reset(&lp);
    HciLpUartRdyEvent(&lp, true);
    Expect("a request readies the receiver before answering",
           "rx=on rdy=0 rdy=release rdy=watch-fall");

    HciLpUartRdyEvent(&lp, false);
    Expect("the end stops the receiver and rearms the level",
           "rx=off rdy=watch-level");

    /*
     * A peer that asks and then goes quiet. Without a timeout the wire stays
     * requested forever and this side can never send again, so the request is
     * given back and counted.
     */
    Reset(&lp);
    assert(HciLpUartSend(&lp, payload, sizeof(payload)));
    gLog.clear();
    gNowMs = 9U;
    HciLpUartProcess(&lp);
    assert(gLog.empty());
    assert(HciLpUartSendBusy(&lp));
    gNowMs = 10U;
    HciLpUartProcess(&lp);
    Expect("an unanswered request is given back", "req=0");
    assert(!HciLpUartSendBusy(&lp));
    assert(lp.AckTimeoutCount == 1U);

    /* And the wire is usable again straight after. */
    assert(HciLpUartSend(&lp, payload, sizeof(payload)));
    Expect("a timed out wire can be requested again", "req=1 req=release");

    /*
     * The timeout has to survive the millisecond counter wrapping, or a board
     * up for 49 days stops being able to send.
     */
    Reset(&lp);
    gNowMs = 0xFFFFFFFAU;
    assert(HciLpUartSend(&lp, payload, sizeof(payload)));
    gLog.clear();
    gNowMs = 0x00000003U;      /* nine ms later, across the wrap */
    HciLpUartProcess(&lp);
    assert(gLog.empty());
    gNowMs = 0x00000004U;      /* ten */
    HciLpUartProcess(&lp);
    Expect("the timeout survives the counter wrapping", "req=0");

    /*
     * Events that do not belong to the state the machine is in. A wire shared
     * between a request and an end of transfer will produce them, and acting
     * on one would put bytes on a wire nobody is listening to.
     */
    Reset(&lp);
    HciLpUartReqFell(&lp);
    assert(gLog.empty());
    HciLpUartSendDone(&lp);
    assert(gLog.empty());
    HciLpUartRdyEvent(&lp, true);
    gLog.clear();
    HciLpUartRdyEvent(&lp, true);   /* already receiving */
    assert(gLog.empty());
    assert(lp.UnexpectedEventCount == 3U);
    printf("[ok] %-44s %u counted\n", "events out of turn are ignored",
           (unsigned)lp.UnexpectedEventCount);

    /* An end seen while idle rearms the level rather than leaving it armed. */
    Reset(&lp);
    HciLpUartRdyEvent(&lp, false);
    Expect("an end while idle puts the watch back", "rdy=watch-level");

    /*
     * A port that cannot start the transfer leaves the wire driven high with
     * the peer waiting, so the failure has to fall back to idle.
     */
    Reset(&lp);
    gSendOk = false;
    assert(HciLpUartSend(&lp, payload, sizeof(payload)));
    gLog.clear();
    HciLpUartReqFell(&lp);
    Expect("a refused send releases the wire", "req=1 send=4 req=0");
    assert(lp.SendErrorCount == 1U);
    assert(!HciLpUartSendBusy(&lp));

    /* A port that leaves its receiver on is still correct on the wire. */
    {
        HciLpUart_t always;
        gLog.clear();
        gNowMs = 0U;
        HciLpUartOps_t ops = MakeOps();
        ops.RxEnable = NULL;
        assert(HciLpUartInit(&always, &ops));
        gLog.clear();
        HciLpUartRdyEvent(&always, true);
        Expect("a port with no receiver control still answers",
               "rdy=0 rdy=release rdy=watch-fall");
    }

    printf("All LPUART handshake tests passed.\n");
    return 0;
}

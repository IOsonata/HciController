/*
 * Standard SysLog integration for HciController developer trace.
 */

#include "hci_trace.h"
#include "device_intrf.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char s_Output[4096];
static size_t s_OutputLen;
static int s_TxCalls;

static void SinkDisable(DevIntrf_t *) {}
static void SinkEnable(DevIntrf_t *) {}
static uint32_t SinkGetRate(DevIntrf_t *) { return 0U; }
static uint32_t SinkSetRate(DevIntrf_t *, uint32_t) { return 0U; }
static bool SinkStartRx(DevIntrf_t *, uint32_t) { return false; }
static int SinkRxData(DevIntrf_t *, uint8_t *, int) { return 0; }
static void SinkStopRx(DevIntrf_t *) {}
static bool SinkStartTx(DevIntrf_t *, uint32_t) { return true; }
static int SinkTxData(DevIntrf_t *, const uint8_t *pData, int DataLen)
{
    assert(pData != nullptr);
    assert(DataLen > 0);
    assert(s_OutputLen + (size_t)DataLen < sizeof(s_Output));
    memcpy(&s_Output[s_OutputLen], pData, (size_t)DataLen);
    s_OutputLen += (size_t)DataLen;
    s_Output[s_OutputLen] = '\0';
    s_TxCalls++;
    return DataLen;
}
static void SinkStopTx(DevIntrf_t *) {}
static void SinkReset(DevIntrf_t *) {}
static void SinkPowerOff(DevIntrf_t *) {}
static void *SinkGetHandle(DevIntrf_t *) { return nullptr; }

static void SinkInit(DevIntrf_t *pSink)
{
    memset(pSink, 0, sizeof(*pSink));
    pSink->Type = DEVINTRF_TYPE_USB;
    pSink->MaxRetry = 0;
    pSink->Disable = SinkDisable;
    pSink->Enable = SinkEnable;
    pSink->GetRate = SinkGetRate;
    pSink->SetRate = SinkSetRate;
    pSink->StartRx = SinkStartRx;
    pSink->RxData = SinkRxData;
    pSink->StopRx = SinkStopRx;
    pSink->StartTx = SinkStartTx;
    pSink->TxData = SinkTxData;
    pSink->TxSrData = SinkTxData;
    pSink->StopTx = SinkStopTx;
    pSink->Reset = SinkReset;
    pSink->PowerOff = SinkPowerOff;
    pSink->GetHandle = SinkGetHandle;
    atomic_flag_clear(&pSink->bBusy);
    atomic_store(&pSink->bTxReady, true);
    atomic_store(&pSink->bNoStop, false);
    atomic_store(&pSink->EnCnt, 1);
}

int main(void)
{
    HciTrace("before transport %u\n", 1U);
    HciTrace("before transport %u\n", 2U);

    DevIntrf_t sink;
    SinkInit(&sink);
    HciTraceSetSink(&sink, 0U);

    assert(s_TxCalls == 0);
    assert(HciTraceFlush() > 0);
    assert(s_TxCalls == 2);

    const char *first = strstr(s_Output, "before transport 1");
    const char *second = strstr(s_Output, "before transport 2");
    assert(first != nullptr);
    assert(second != nullptr);
    assert(first < second);
    assert(HciTraceFlush() == 0);

    HciTrace("with transport %u\n", 3U);
    assert(s_TxCalls == 3);
    assert(strstr(s_Output, "with transport 3") != nullptr);

    HciTraceSetSink(nullptr, 0U);
    HciTrace("detached transport %u\n", 4U);
    assert(s_TxCalls == 3);

    HciTraceSetSink(&sink, 0U);
    assert(s_TxCalls == 3);
    assert(HciTraceFlush() > 0);
    assert(s_TxCalls == 4);
    assert(strstr(s_Output, "detached transport 4") != nullptr);
    assert(HciTraceFlush() == 0);

    printf("HciTrace SysLog integration tests: PASS\n");
    return 0;
}

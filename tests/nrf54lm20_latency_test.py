#!/usr/bin/env python3
"""-------------------------------------------------------------------------
@file nrf54lm20_latency_test.py

@brief Compile the LM20 MPSL callbacks against fake power registers.

Exercise RRAM standby and restoration with and without POWER CONSTLAT,
including overlapping requests through the old and new MPSL callback names.
This checks register transitions, not radio timing on hardware.
@author Nguyen Hoan Hoang
@date October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------"""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "hci_nrf54lm20.cpp"


class Lm20LatencyTest(unittest.TestCase):
    def test_sync_trace(self):
        source = SOURCE.read_text()
        trace = source.split("#if HCI_LM20_SYNC_TRACE", 1)[1].split("#endif", 1)[0]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <string>
static std::string output;
static unsigned flushes;
typedef unsigned IRQn_Type;
enum { RADIO_0_IRQn, GRTC_3_IRQn, TIMER10_IRQn, SWI00_IRQn, CLOCK_POWER_IRQn };
static unsigned NVIC_GetEnableIRQ(IRQn_Type) { return 1; }
static unsigned NVIC_GetPendingIRQ(IRQn_Type irq) { return irq == GRTC_3_IRQn; }
static unsigned NVIC_GetActive(IRQn_Type) { return 0; }
static unsigned __get_PRIMASK() { return 0; }
static unsigned __get_BASEPRI() { return 0; }
struct {
    struct { uint32_t SYSCOUNTERL; uint32_t SYSCOUNTERH; } SYSCOUNTER[4];
    struct { uint32_t CCL; uint32_t CCH; } CC[12];
    uint32_t MODE;
    uint32_t INTENSET3;
    uint32_t EVENTS_COMPARE[12];
} grtc;
#define NRF_GRTC (&grtc)
static int HciTraceFlush() { ++flushes; return 0; }
static uint32_t s_ConstLatRefs;
struct { struct { uint32_t LOWPOWERCONFIG; } POWER; } rram;
#define NRF_RRAMC (&rram)
static void HciTrace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void HciTrace(const char *format, ...)
{
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    output += line;
}
'''
        main = r'''
int main()
{
    uint8_t command[] = {0x44, 0x20, 14};
    uint8_t status[] = {0x0f, 4, 0, 1, 0x44, 0x20};
    uint8_t cancel[] = {0x0e, 4, 1, 0x45, 0x20, 0};
    uint8_t sync[] = {0x3e, 4, 0x24, 0x44, 0x34, 0x12};
    uint8_t ext[] = {0x3e, 1, 0x0d};
    uint8_t report[] = {0x3e, 1, 0x25};
    HciNrf54lm20TraceCommand(command, sizeof(command));
    HciNrf54lm20TraceEvent(status, sizeof(status));
    HciNrf54lm20TraceEvent(cancel, sizeof(cancel));
    HciNrf54lm20TraceEvent(sync, sizeof(sync));
    assert(output.find("dispatch op=2044") != std::string::npos);
    assert(output.find("reply op=2044 status=00 evt=0f") != std::string::npos);
    assert(output.find("reply op=2045 status=00 evt=0e") != std::string::npos);
    assert(output.find("sync sub=24 status=44 handle=1234") != std::string::npos);
    output.clear();
    for (unsigned i = 0; i < 100; ++i) {
        HciNrf54lm20TraceEvent(ext, sizeof(ext));
        HciNrf54lm20TraceEvent(report, sizeof(report));
    }
    // Frequent reports update counters without flooding the trace FIFO.
    assert(output.empty());
    assert(s_TraceExtReports == 100 && s_TracePeriodicReports == 100);
    HciNrf54lm20TraceEvent(nullptr, 0);
    HciNrf54lm20TraceCommand(nullptr, 0);
    HciNrf54lm20TraceProcess();
    assert(output.empty() && flushes == 0);
    HciNrf54lm20TraceRequest();
    HciNrf54lm20TraceRequest();
    // The status thread cannot format or flush unpublished SysLog records.
    assert(output.empty() && flushes == 0);
    grtc.INTENSET3 = 0xf80;
    grtc.EVENTS_COMPARE[7] = 1;
    grtc.CC[7].CCL = 0x12345678;
    HciNrf54lm20TraceProcess();
    assert(flushes == 1);
    assert(output.find("snap=1 nvic en=1f pend=02 active=00") != std::string::npos);
    assert(output.find("inten3=f80 events=80") != std::string::npos);
    assert(output.find("cc7=00000000:12345678") != std::string::npos);
    assert(grtc.INTENSET3 == 0xf80 && grtc.EVENTS_COMPARE[7] == 1);
    HciNrf54lm20TraceProcess();
    assert(flushes == 1);
    assert(output.find("ext=100 periodic=100") != std::string::npos);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "trace.cpp"
            binary = Path(directory) / "trace"
            cpp.write_text(harness + trace + main)
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++11",
                "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_rram_lifetime(self):
        source = SOURCE.read_text()
        callbacks = source.split(
            "/* nRF54L MPSL low-latency window callbacks. */", 1)[1]
        declarations = "\n".join(
            line for line in source.splitlines()
            if line.startswith("static ")
            and ("s_ConstLatRefs;" in line or "s_RramLowPowerConfig;" in line)
        )
        prefix = r'''
#include <cassert>
#include <cstdint>
struct RramRegisters {
    struct { uint32_t LOWPOWERCONFIG; } POWER;
};
static RramRegisters rram;
#define NRF_RRAMC (&rram)
#define RRAMC_POWER_LOWPOWERCONFIG_MODE_Standby 1U
#define RRAMC_POWER_LOWPOWERCONFIG_MODE_Pos 0U
#define NRF_POWER nullptr
#define NRF_POWER_TASK_CONSTLAT 1U
#define NRF_POWER_TASK_LOWPWR 2U
#if NRF_POWER_HAS_CONST_LATENCY
static unsigned requests;
static unsigned releases;
static void nrf_power_task_trigger(void *, unsigned task)
{
    if (task == NRF_POWER_TASK_CONSTLAT) ++requests;
    else if (task == NRF_POWER_TASK_LOWPWR) ++releases;
    else assert(false);
}
#endif
'''
        main = r'''
int main()
{
    const uint32_t original = 0xA503U;
    rram.POWER.LOWPOWERCONFIG = original;
    mpsl_low_latency_release_callback();
    assert(rram.POWER.LOWPOWERCONFIG == original);
    for (unsigned i = 0; i < 100; ++i) {
        mpsl_low_latency_acquire_callback();
        assert(rram.POWER.LOWPOWERCONFIG == 1U);
        mpsl_constlat_request_callback();
        mpsl_low_latency_release_callback();
        assert(rram.POWER.LOWPOWERCONFIG == 1U);
        mpsl_lowpower_request_callback();
        assert(rram.POWER.LOWPOWERCONFIG == original);
        mpsl_lowpower_request_callback();
        assert(rram.POWER.LOWPOWERCONFIG == original);
    }
    // A subsequent window must save the current policy, not the first one.
    rram.POWER.LOWPOWERCONFIG = 0U;
    mpsl_constlat_request_callback();
    assert(rram.POWER.LOWPOWERCONFIG == 1U);
    mpsl_low_latency_release_callback();
    assert(rram.POWER.LOWPOWERCONFIG == 0U);
#if NRF_POWER_HAS_CONST_LATENCY
    assert(requests == 101U && releases == 101U);
#endif
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            cpp = path / "latency.cpp"
            cpp.write_text(prefix + declarations + callbacks + main)
            for has_constlat in (0, 1):
                with self.subTest(power_constlat=has_constlat):
                    binary = path / ("latency_%u" % has_constlat)
                    subprocess.run([
                        os.environ.get("CXX", "c++"), "-std=c++11",
                        "-Wall", "-Wextra", "-Werror",
                        "-DNRF_POWER_HAS_CONST_LATENCY=%u" % has_constlat,
                        str(cpp), "-o", str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

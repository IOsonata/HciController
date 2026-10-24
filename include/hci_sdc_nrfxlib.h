/**-------------------------------------------------------------------------
@file	hci_sdc_nrfxlib.h

@brief	nrfxlib SoftDevice Controller HCI command adapter declarations.

		Declares initialization of the nrfxlib command table, startup No
		Operation event support, and advertising-command-set reset handling.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_SDC_NRFXLIB_H
#define HCI_SDC_NRFXLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_counters.h"
#include "hci_sdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * pCounters becomes the command context, which only the counter readout uses.
 * NULL is allowed and makes that one command answer Command Disallowed; every
 * other row in the table is unaffected.
 */
bool HciSdcNrfxlibInit(HciSdc_t *pSdc,
                       uint8_t *pCommandEvent,
                       size_t CommandEventCapacity,
                       HciCounters_t *pCounters);

/*
 * Queue the No Operation Command Complete that says the controller is ready,
 * Vol 4 Part E 7.7.14 with opcode 0x0000. Whether a board needs it is a board
 * fact, so the caller decides: hci_sdc_nrfxlib.cpp cannot see board.h, and a
 * macro tested there gave the one board that sets it an image with the call
 * compiled out.
 *
 * Call after HciSdcNrfxlibInit and before the runtime thread starts.
 */
void HciSdcNrfxlibQueueStartupNop(HciSdc_t *pSdc);

/*
 * Give up the advertising command set the host has chosen.
 *
 * Vol 4 Part E 3.1.1 lets a host use the legacy advertising, scanning and
 * initiating commands or the extended ones, not both, and ties the choice to
 * the last reset. The dispatch layer refuses the second set, so this is what
 * HCI Reset calls, and what a test that walks the whole table needs between
 * rows.
 */
void HciSdcNrfxlibResetAdvCommandType(void);

#ifdef __cplusplus
}

/*
 * The SoftDevice Controller as an IOsonata interface object: the packet
 * DeviceIntrf the bridge drives, DevAddr the packet type, over the nrfxlib
 * controller. Init takes what HciSdcNrfxlibInit takes.
 */
class HciSdcNrfxlib : public DeviceIntrf {
public:
    HciSdcNrfxlib() = default;
    HciSdcNrfxlib(const HciSdcNrfxlib &) = delete;
    HciSdcNrfxlib &operator = (const HciSdcNrfxlib &) = delete;

    bool Init(uint8_t *pCommandEvent, size_t CommandEventCapacity, HciCounters_t *pCounters) {
        return HciSdcNrfxlibInit(&vDevData, pCommandEvent, CommandEventCapacity, pCounters);
    }
    bool KnowsCommand(uint16_t Opcode, size_t ParamLen) { return HciSdcKnowsCommand(&vDevData, Opcode, ParamLen); }

    operator DevIntrf_t * () override { return &vDevData.DevIntrf; }
    DevIntrf_t *Data(void) { return &vDevData.DevIntrf; }
    operator HciSdc_t * () { return &vDevData; }
    operator const HciSdc_t * () const { return &vDevData; }

    uint32_t Rate(uint32_t DataRate) override { return DeviceIntrfSetRate(&vDevData.DevIntrf, DataRate); }
    uint32_t Rate(void) override { return DeviceIntrfGetRate(&vDevData.DevIntrf); }
    void Disable(void) override { DeviceIntrfDisable(&vDevData.DevIntrf); }
    void Enable(void) override { DeviceIntrfEnable(&vDevData.DevIntrf); }
    bool StartRx(uint32_t DevAddr) override { return DeviceIntrfStartRx(&vDevData.DevIntrf, DevAddr); }
    void StopRx(void) override { DeviceIntrfStopRx(&vDevData.DevIntrf); }
    bool StartTx(uint32_t DevAddr) override { return DeviceIntrfStartTx(&vDevData.DevIntrf, DevAddr); }
    void StopTx(void) override { DeviceIntrfStopTx(&vDevData.DevIntrf); }
    int Tx(uint32_t DevAddr, const uint8_t *pData, int DataLen) override {
        return DeviceIntrfTx(&vDevData.DevIntrf, DevAddr, pData, DataLen);
    }
    int Rx(uint32_t DevAddr, uint8_t *pBuff, int BuffLen) override {
        return DeviceIntrfRx(&vDevData.DevIntrf, DevAddr, pBuff, BuffLen);
    }
    int TxData(const uint8_t *pData, int DataLen) override {
        return DeviceIntrfTxData(&vDevData.DevIntrf, pData, DataLen);
    }
    int RxData(uint8_t *pBuff, int BuffLen) override {
        return DeviceIntrfRxData(&vDevData.DevIntrf, pBuff, BuffLen);
    }

private:
    HciSdc_t vDevData = {};
};
#endif

#endif /* HCI_SDC_NRFXLIB_H */

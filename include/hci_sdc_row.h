/**-------------------------------------------------------------------------
@file	hci_sdc_row.h

@brief	Command table rows that pass straight through to one SDC call.

		Shared by hci_sdc_nrfxlib.cpp and the feature files that bring their
		own rows, such as hci_sdc_feat_cs.cpp. C++ only.

@author	Nguyen Hoan Hoang
@date	November 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_SDC_ROW_H
#define HCI_SDC_ROW_H

#include <stddef.h>
#include <stdint.h>

#include "hci_cmd_dispatch.h"

/*
 * The length a row gives is checked against the SDC structure at compile
 * time, and so is the size of the buffers the shared path keeps.
 */
template <size_t Size, size_t Given>
constexpr uint16_t HciSdcRowLen()
{
    static_assert(Size == Given, "row length differs from the SDC structure");
    static_assert(Size <= HCI_CMD_GENERIC_IN_MAX, "structure too large for a shared row");
    return (uint16_t)Size;
}

template <size_t Given>
constexpr uint16_t HciSdcRowLenN()
{
    static_assert(Given == 0, "a command without parameters has no length");
    return 0U;
}

template <size_t Size>
constexpr uint16_t HciSdcRowRet()
{
    static_assert(Size <= HCI_CMD_GENERIC_OUT_MAX, "return too large for a shared row");
    return (uint16_t)Size;
}

#define HCI_SDC_ROW_FN(Func) \
    reinterpret_cast<HciCmdHandler_t>(reinterpret_cast<void (*)(void)>(Func))

#define HCI_SDC_ROW_P(Opcode, Resp, SdcType, ParamLen, Func)                  \
    {Opcode, HciSdcRowLen<sizeof(SdcType), (ParamLen)>(), 0U, Resp,           \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_P}

#define HCI_SDC_ROW_PR(Opcode, SdcType, ParamLen, SdcReturn, Func)            \
    {Opcode, HciSdcRowLen<sizeof(SdcType), (ParamLen)>(),                     \
     HciSdcRowRet<sizeof(SdcReturn)>(), HCI_CMD_RESPONSE_COMPLETE,            \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_PR}

#define HCI_SDC_ROW_N(Opcode, Resp, ParamLen, Func)                           \
    {Opcode, HciSdcRowLenN<(ParamLen)>(), 0U, Resp,                           \
     HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_N}

#define HCI_SDC_ROW_NR(Opcode, ParamLen, SdcReturn, Func)                     \
    {Opcode, HciSdcRowLenN<(ParamLen)>(), HciSdcRowRet<sizeof(SdcReturn)>(),  \
     HCI_CMD_RESPONSE_COMPLETE, HCI_SDC_ROW_FN(Func), HCI_CMD_KIND_NR}

#endif /* HCI_SDC_ROW_H */

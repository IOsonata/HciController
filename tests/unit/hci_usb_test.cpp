/* Native Bluetooth HCI class tests over the IOsonata USB interface. */

#include "hci_usb.h"
#include "hci_h4.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	uint8_t *pBuffer;
	UsbCtrlrEpHandler_t Handler;
	void *pContext;
	uint16_t Length;
	bool Busy;
} FakeEp_t;

static UsbCfg_t s_UsbCfg;
static UsbFuncCfg_t s_Function;
static FakeEp_t s_Ep[256];
static bool s_Configured;

extern "C" {
const UsbCfg_t *UsbGetCfg(int DevNo)
{
	return DevNo == 0 ? &s_UsbCfg : nullptr;
}

const char *UsbGetSerial(int DevNo)
{
	return DevNo == 0 ? s_UsbCfg.pSerial : nullptr;
}

bool UsbRegisterFunc(int DevNo, const UsbFuncCfg_t *pCfg)
{
	if (DevNo != 0 || pCfg == nullptr ||
		(((pCfg->EpInMask | pCfg->EpOutMask) & 1U) != 0U) ||
		((pCfg->EpInMask | pCfg->EpOutMask) != 0U &&
		 pCfg->XferHandler == nullptr))
	{
		return false;
	}
	s_Function = *pCfg;
	return true;
}

bool UsbConfigured(int DevNo) { return DevNo == 0 && s_Configured; }
bool UsbCtrlrHighSpeed(int) { return false; }
bool UsbCtrlrEpOpen(int, const UsbEndPointDesc_t *) { return true; }
void UsbCtrlrEpClose(int, uint8_t EpAddr) { s_Ep[EpAddr].Busy = false; }

bool UsbCtrlrEpRegister(int, uint8_t EpAddr, uint8_t *pBuffer,
						UsbCtrlrEpHandler_t Handler, void *pContext)
{
	s_Ep[EpAddr].pBuffer = pBuffer;
	s_Ep[EpAddr].Handler = Handler;
	s_Ep[EpAddr].pContext = pContext;
	return true;
}

bool UsbCtrlrEpRxArm(int, uint8_t EpNo)
{
	FakeEp_t *pEp = &s_Ep[USB_ENDPADDR_DIROUT(EpNo)];
	if (pEp->Busy)
	{
		return false;
	}
	pEp->Busy = true;
	return true;
}

bool UsbCtrlrEpSend(int, uint8_t EpNo, uint16_t Length)
{
	FakeEp_t *pEp = &s_Ep[USB_ENDPADDR_DIRIN(EpNo)];
	if (pEp->Busy)
	{
		return false;
	}
	pEp->Busy = true;
	pEp->Length = Length;
	return true;
}

void DeviceIntrfEnable(DevIntrf_t *pDev)
{
	atomic_fetch_add(&pDev->EnCnt, 1);
	pDev->Enable(pDev);
}

void DeviceIntrfDisable(DevIntrf_t *pDev)
{
	if (atomic_load(&pDev->EnCnt) > 0)
	{
		atomic_fetch_sub(&pDev->EnCnt, 1);
	}
	pDev->Disable(pDev);
}

int DeviceIntrfRx(DevIntrf_t *pDev, uint32_t DevAddr,
				   uint8_t *pData, int Length)
{
	if (!DeviceIntrfStartRx(pDev, DevAddr))
	{
		return 0;
	}
	const int count = DeviceIntrfRxData(pDev, pData, Length);
	DeviceIntrfStopRx(pDev);
	return count;
}

int DeviceIntrfTx(DevIntrf_t *pDev, uint32_t DevAddr,
				   const uint8_t *pData, int Length)
{
	if (!DeviceIntrfStartTx(pDev, DevAddr))
	{
		return 0;
	}
	const int count = DeviceIntrfTxData(pDev, pData, Length);
	DeviceIntrfStopTx(pDev);
	return count;
}
}

static void ResetFake(void)
{
	memset(&s_UsbCfg, 0, sizeof(s_UsbCfg));
	memset(&s_Function, 0, sizeof(s_Function));
	memset(s_Ep, 0, sizeof(s_Ep));
	s_UsbCfg.DevNo = 0;
	s_UsbCfg.Vid = HciUsbDescriptorVid();
	s_UsbCfg.Pid = HciUsbDescriptorPid(HCI_USB_DESCRIPTOR_NATIVE_HCI);
	s_UsbCfg.DevVer = 0x0100U;
	s_UsbCfg.pManufacturer = "I-SYST inc.";
	s_UsbCfg.pProduct = "HciController";
	s_UsbCfg.pSerial = "01234567";
	s_Configured = true;
}

static void CompleteOut(uint8_t EpNo, const uint8_t *pData, uint16_t Length)
{
	FakeEp_t *pEp = &s_Ep[USB_ENDPADDR_DIROUT(EpNo)];
	assert(pEp->Busy && pEp->Handler != nullptr);
	memcpy(pEp->pBuffer, pData, Length);
	pEp->Busy = false;
	pEp->Handler(USB_ENDPADDR_DIROUT(EpNo), Length,
				 USB_CTRLR_XFER_SUCCESS, pEp->pContext);
}

static void CompleteIn(uint8_t EpNo)
{
	FakeEp_t *pEp = &s_Ep[USB_ENDPADDR_DIRIN(EpNo)];
	assert(pEp->Busy && pEp->Handler != nullptr);
	const uint16_t length = pEp->Length;
	pEp->Busy = false;
	pEp->Handler(USB_ENDPADDR_DIRIN(EpNo), length,
				 USB_CTRLR_XFER_SUCCESS, pEp->pContext);
}

static void CheckDescriptors(const UsbdHciDesc_t *pHci)
{
	assert(HciUsbDescriptorSetMode(HCI_USB_DESCRIPTOR_NATIVE_HCI));
	assert(HciUsbDescriptorSetHci(pHci));
	uint16_t length = 0U;
	const uint8_t *p = HciUsbDescHandler(USB_DESCTYPE_DEVICE, 0U, 0U,
		USB_SPEED_FULL, &length, nullptr);
	assert(p != nullptr && length == sizeof(UsbDevDesc_t));
	const UsbDevDesc_t *pDevice = reinterpret_cast<const UsbDevDesc_t *>(p);
	assert(pDevice->idVendor == HciUsbDescriptorVid());
	assert(pDevice->idProduct ==
		HciUsbDescriptorPid(HCI_USB_DESCRIPTOR_NATIVE_HCI));

	p = HciUsbDescHandler(USB_DESCTYPE_CONFIGURATION, 0U, 0U,
		USB_SPEED_FULL, &length, nullptr);
	assert(p != nullptr && length > 9U);
	assert(((uint16_t)p[2] | ((uint16_t)p[3] << 8)) == length);
	assert(p[4] == 4U);
	assert(memcmp(&p[9], pHci, sizeof(*pHci)) == 0);
	const size_t log = 9U + sizeof(*pHci);
	assert(p[log] == 8U && p[log + 1U] == USB_DESCTYPE_IA);
	assert(p[log + 2U] == 2U);
	assert(p[log + 38U] == USB_ENDPADDR_DIRIN(3U));
	assert(p[log + 54U] == USB_ENDPADDR_DIROUT(4U));
	assert(p[log + 61U] == USB_ENDPADDR_DIRIN(4U));
}

int main(void)
{
	ResetFake();

	alignas(4) uint8_t rxMem[CFIFO_TOTAL_MEMSIZE(8U, HCI_USB_PKT_BLKSIZE)];
	alignas(4) uint8_t txMem[CFIFO_TOTAL_MEMSIZE(20U, HCI_USB_PKT_BLKSIZE)];
	UsbdHciCfg_t cfg = {};
	cfg.bBlocking = true;
	cfg.RxFifoMemSize = sizeof(rxMem);
	cfg.pRxFifoMem = rxMem;
	cfg.TxFifoMemSize = sizeof(txMem);
	cfg.pTxFifoMem = txMem;
	cfg.DevNo = 0;
	cfg.InterfaceString = HCI_USB_STRING_BT;

	UsbdHci usb;
	assert(usb.Init(cfg));
	UsbdHciDesc_t hciDesc = {};
	assert(usb.MakeDesc(&hciDesc, USB_SPEED_FULL));
	CheckDescriptors(&hciDesc);
	const uint8_t eventEp = USB_ENDPADDR_NUM(hciDesc.EventIn.bEndpointAddress);
	const uint8_t aclEp = USB_ENDPADDR_NUM(hciDesc.AclIn.bEndpointAddress);
	assert(s_Function.FirstInterface == 0U);
	assert(s_Function.InterfaceCount == 2U);
	assert(s_Function.EpInMask == 0x0006U);
	assert(s_Function.EpOutMask == 0x0004U);
	assert(s_Function.ConfigHandler(1U, s_Function.pContext));
	assert(s_Ep[USB_ENDPADDR_DIROUT(aclEp)].Busy);

	UsbSetupData_t request = {};
	request.bmRequestType = USB_REQTYPE_DIRDEV | USB_REQTYPE_CLASS |
		USB_REQTYPE_INTERFACE;
	request.bRequest = 0U;
	request.wIndex = 0U;
	request.wLength = 3U;
	uint8_t *pControl = nullptr;
	uint16_t controlLength = 0U;
	assert(s_Function.RequestHandler(&request, USB_CTRL_SETUP, &pControl,
		&controlLength, s_Function.pContext));
	const uint8_t command[] = { 0x03U, 0x0CU, 0x00U };
	memcpy(pControl, command, sizeof(command));
	controlLength = sizeof(command);
	assert(s_Function.RequestHandler(&request, USB_CTRL_DATA, &pControl,
		&controlLength, s_Function.pContext));
	assert(s_Function.RequestHandler(&request, USB_CTRL_COMPLETE, &pControl,
		&controlLength, s_Function.pContext));
	uint8_t received[80] = {};
	assert(DeviceIntrfRx(usb.Data(), HCI_H4_PACKET_COMMAND, received,
		sizeof(received)) == (int)sizeof(command));
	assert(memcmp(received, command, sizeof(command)) == 0);

	const uint8_t aclOut[] = { 1U, 0U, 3U, 0U, 0xA1U, 0xA2U, 0xA3U };
	CompleteOut(aclEp, aclOut, sizeof(aclOut));
	assert(DeviceIntrfRx(usb.Data(), HCI_H4_PACKET_ACL, received,
		sizeof(received)) == (int)sizeof(aclOut));
	assert(memcmp(received, aclOut, sizeof(aclOut)) == 0);

	uint8_t longAclOut[74] = { 2U, 0U, 70U, 0U };
	for (size_t i = 4U; i < sizeof(longAclOut); i++)
	{
		longAclOut[i] = (uint8_t)(0x80U + i);
	}
	CompleteOut(aclEp, longAclOut, 64U);
	assert(DeviceIntrfRx(usb.Data(), HCI_H4_PACKET_ACL, received,
		sizeof(received)) == 0);
	CompleteOut(aclEp, &longAclOut[64], 10U);
	assert(DeviceIntrfRx(usb.Data(), HCI_H4_PACKET_ACL, received,
		sizeof(received)) == (int)sizeof(longAclOut));
	assert(memcmp(received, longAclOut, sizeof(longAclOut)) == 0);

	uint8_t fullAclOut[HCI_USB_FS_BULK_MPS] = { 3U, 0U,
		HCI_USB_FS_BULK_MPS - 4U, 0U };
	for (size_t i = 4U; i < sizeof(fullAclOut); i++)
	{
		fullAclOut[i] = (uint8_t)(0x60U + i);
	}
	CompleteOut(aclEp, fullAclOut, sizeof(fullAclOut));
	assert(DeviceIntrfRx(usb.Data(), HCI_H4_PACKET_ACL, received,
		sizeof(received)) == (int)sizeof(fullAclOut));
	assert(memcmp(received, fullAclOut, sizeof(fullAclOut)) == 0);
	CompleteOut(aclEp, fullAclOut, 0U);

	const uint8_t event[] = { 0x0EU, 0x02U, 0x01U, 0x00U };
	assert(DeviceIntrfTx(usb.Data(), HCI_H4_PACKET_EVENT, event,
		sizeof(event)) == (int)sizeof(event));
	assert(s_Ep[0x81U].Busy && s_Ep[0x81U].Length == sizeof(event));
	assert(memcmp(s_Ep[0x81U].pBuffer, event, sizeof(event)) == 0);
	CompleteIn(eventEp);

	uint8_t longEvent[20U] = { 0x0EU, 18U };
	for (size_t i = 2U; i < sizeof(longEvent); i++)
	{
		longEvent[i] = (uint8_t)(0x20U + i);
	}
	assert(DeviceIntrfTx(usb.Data(), HCI_H4_PACKET_EVENT, longEvent,
		sizeof(longEvent)) == (int)sizeof(longEvent));
	assert(s_Ep[0x81U].Busy && s_Ep[0x81U].Length == HCI_USB_EVENT_MPS);
	assert(memcmp(s_Ep[0x81U].pBuffer, longEvent, HCI_USB_EVENT_MPS) == 0);
	CompleteIn(eventEp);
	assert(s_Ep[0x81U].Busy && s_Ep[0x81U].Length == 4U);
	assert(memcmp(s_Ep[0x81U].pBuffer,
		&longEvent[HCI_USB_EVENT_MPS], 4U) == 0);
	CompleteIn(eventEp);

	uint8_t fullEvent[HCI_USB_EVENT_MPS] = { 0x0EU,
		HCI_USB_EVENT_MPS - 2U };
	for (size_t i = 2U; i < sizeof(fullEvent); i++)
	{
		fullEvent[i] = (uint8_t)i;
	}
	assert(DeviceIntrfTx(usb.Data(), HCI_H4_PACKET_EVENT, fullEvent,
		sizeof(fullEvent)) == (int)sizeof(fullEvent));
	assert(s_Ep[0x81U].Busy && s_Ep[0x81U].Length == sizeof(fullEvent));
	CompleteIn(eventEp);
	assert(s_Ep[0x81U].Busy && s_Ep[0x81U].Length == 0U);
	CompleteIn(eventEp);

	uint8_t aclIn[70] = { 1U, 0U, 66U, 0U };
	for (size_t i = 4U; i < sizeof(aclIn); i++)
	{
		aclIn[i] = (uint8_t)i;
	}
	assert(DeviceIntrfTx(usb.Data(), HCI_H4_PACKET_ACL, aclIn,
		sizeof(aclIn)) == (int)sizeof(aclIn));
	assert(s_Ep[0x82U].Busy && s_Ep[0x82U].Length == 64U);
	assert(memcmp(s_Ep[0x82U].pBuffer, aclIn, 64U) == 0);
	CompleteIn(aclEp);
	assert(s_Ep[0x82U].Busy && s_Ep[0x82U].Length == 6U);
	assert(memcmp(s_Ep[0x82U].pBuffer, &aclIn[64], 6U) == 0);
	CompleteIn(aclEp);

	uint8_t fullAclIn[HCI_USB_FS_BULK_MPS] = { 3U, 0U,
		HCI_USB_FS_BULK_MPS - 4U, 0U };
	for (size_t i = 4U; i < sizeof(fullAclIn); i++)
	{
		fullAclIn[i] = (uint8_t)(0x40U + i);
	}
	assert(DeviceIntrfTx(usb.Data(), HCI_H4_PACKET_ACL, fullAclIn,
		sizeof(fullAclIn)) == (int)sizeof(fullAclIn));
	assert(s_Ep[0x82U].Busy && s_Ep[0x82U].Length == sizeof(fullAclIn));
	CompleteIn(aclEp);
	assert(s_Ep[0x82U].Busy && s_Ep[0x82U].Length == 0U);
	CompleteIn(aclEp);

	assert(!s_Function.SetInterfaceHandler(0U, 1U, s_Function.pContext));

	puts("hci_usb_test: pass");
	return 0;
}

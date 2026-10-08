/* Native Bluetooth HCI composite topology over IOsonata main USB core.
 * LE-only controller: no SCO alternates. */

#include "hci_usb.h"
#include "usb/usbd_cdc.h"

// Exercise the application setup and worker with the real IOsonata stack.
// Unused radio/application entry points are discarded by the test link.
#include "../../src/hci_app.cpp"

#include "app_evt_handler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int s_RegisteredEpCount;
static bool s_CtrlrInitialized;
static unsigned s_WorkWakeCount;
static unsigned s_ProcessCount;
static unsigned s_StartCount;
static bool s_Vbus = true;

extern "C" void HciTaktOsWake(HciTaktOs_t *, uint32_t)
{
	s_WorkWakeCount++;
}

extern "C" {
void AppWait(void) {}
bool UsbCtrlrInit(int DevNo, const UsbCtrlrCfg_t *pCfg)
{
	if (DevNo != 0 || pCfg == nullptr)
	{
		return false;
	}
	s_CtrlrInitialized = true;
	return true;
}

bool UsbCtrlrStart(int DevNo) { s_StartCount++; return DevNo == 0 && s_Vbus; }
void UsbCtrlrStop(int) {}
void UsbCtrlrProcess(int) { s_ProcessCount++; }
bool UsbCtrlrVbusDetected(int DevNo) { return DevNo == 0; }
bool UsbCtrlrHighSpeed(int) { return false; }
void UsbCtrlrIntEnable(int) {}
void UsbCtrlrIntDisable(int) {}
void UsbCtrlrConnect(int) {}
void UsbCtrlrDisconnect(int) {}
void UsbCtrlrRemoteWakeup(int) {}
void UsbCtrlrSofEnable(int, bool) {}
void UsbCtrlrSetAddress(int, uint8_t) {}
bool UsbCtrlrEpOpen(int DevNo, const UsbEndPointDesc_t *pDesc)
{
	return DevNo == 0 && pDesc != nullptr;
}
void UsbCtrlrEpClose(int, uint8_t, bool) {}
void UsbCtrlrEpCloseAll(int) {}

void UsbCtrlrEpBind(int DevNo, uint8_t, bool, bool,
					 UsbCtrlrEpHandler_t Handler, void *)
{
	if (DevNo == 0 && Handler != nullptr)
	{
		s_RegisteredEpCount++;
	}
}

bool UsbCtrlrEpReceive(int DevNo, uint8_t, uint8_t *pBuffer, uint16_t)
{
	return DevNo == 0 && pBuffer != nullptr;
}

void UsbCtrlrEpProcessEvent(int, uint8_t, bool, UsbCtrlrEvtType_t, uint16_t) {}
bool UsbCtrlrEpSend(int DevNo, uint8_t, uint8_t *, uint16_t) { return DevNo == 0; }
bool UsbCtrlrIsoSend(int DevNo, uint8_t, uint8_t *, uint16_t) { return DevNo == 0; }
uint16_t UsbCtrlrIsoTraceSnapshot(int, uint8_t **ppData)
{
	if (ppData != nullptr)
	{
		*ppData = nullptr;
	}
	return 0U;
}
int UsbCtrlrEp0Send(int DevNo, uint8_t *, int Length)
{
	return DevNo == 0 ? Length : -1;
}
bool UsbCtrlrEp0Status(int DevNo, uint8_t) { return DevNo == 0; }
void UsbCtrlrEpStall(int, uint8_t, bool) {}
void UsbCtrlrEpClearStall(int, uint8_t, bool) {}
size_t UsbCtrlrGetSerial(int DevNo, char *pBuff, size_t BuffLen)
{
	static const char serial[] = "01234567";
	if (DevNo != 0 || pBuff == nullptr || BuffLen == 0U)
	{
		return 0U;
	}
	const size_t len = sizeof(serial) - 1U;
	const size_t copy = len < BuffLen - 1U ? len : BuffLen - 1U;
	memcpy(pBuff, serial, copy);
	pBuff[copy] = 0;
	return copy;
}
}

static void CheckHci(const BtHciUsbSerialDesc_t *pHci)
{
	assert(pHci != nullptr);
	assert(pHci->Association.bFirstInterface == 0U);
	assert(pHci->Association.bInterfaceCount == 2U);
	assert(pHci->Hci.bInterfaceNumber == 0U);
	assert(pHci->Hci.bAlternateSetting == 0U);
	assert(pHci->Hci.bNumEndpoints == 3U);
	assert(pHci->EventIn.bEndpointAddress == USB_ENDPADDR_DIRIN(1U));
	assert(pHci->AclOut.bEndpointAddress == USB_ENDPADDR_DIROUT(2U));
	assert(pHci->AclIn.bEndpointAddress == USB_ENDPADDR_DIRIN(2U));
	assert(pHci->Serialized.Interface.bInterfaceNumber == 0U);
	assert(pHci->Serialized.Interface.bAlternateSetting == 1U);
	assert(pHci->Serialized.Interface.bNumEndpoints == 2U);
	assert(pHci->Serialized.Out.bEndpointAddress ==
		pHci->AclOut.bEndpointAddress);
	assert(pHci->Serialized.In.bEndpointAddress ==
		pHci->AclIn.bEndpointAddress);
	assert(pHci->Sync.bInterfaceNumber == 1U);
	assert(pHci->Sync.bAlternateSetting == 0U);
	assert(pHci->Sync.bNumEndpoints == 0U);
}

static unsigned s_WorkCount;
static unsigned s_RequeueRemaining;

static void Work(uint32_t EvtId, void *pCtx)
{
	assert(pCtx == &s_WorkCount);
	assert(EvtId == s_WorkCount);
	s_WorkCount++;
	if (s_RequeueRemaining != 0U)
	{
		s_RequeueRemaining--;
		assert(UsbEvtQue(s_WorkCount, pCtx, Work));
	}
}

static void CheckWorkerQueue(HciApp_t &App)
{
	assert(!UsbEvtQue(0U, nullptr, nullptr));
	s_WorkCount = 0U;
	for (unsigned i = 0; i < HCI_USB_WORK_COUNT; i++)
		assert(UsbEvtQue(i, &s_WorkCount, Work));
	assert(!UsbEvtQue(HCI_USB_WORK_COUNT, &s_WorkCount, Work));
	assert(s_WorkCount == 0U); // Producers must never execute callbacks.
	const unsigned processed = s_ProcessCount;
	UsbProcessQue(0); // Full queue: IOsonata owes the process event.
	HciAppUsbWorkExec();
	assert(s_WorkCount == HCI_USB_WORK_COUNT);
	assert(CFifoPeek(s_hUsbWork) != nullptr);
	HciAppUsbWorkExec();
	assert(s_ProcessCount == processed + 1U);

	s_WorkCount = 0U;
	s_RequeueRemaining = HCI_USB_WORK_PER_PASS + 4U;
	assert(UsbEvtQue(0U, &s_WorkCount, Work));
	HciAppUsbWorkExec();
	assert(s_WorkCount == HCI_USB_WORK_PER_PASS);
	assert(CFifoPeek(s_hUsbWork) != nullptr);
	HciAppUsbWorkExec();
	assert(s_WorkCount == HCI_USB_WORK_PER_PASS + 5U);
	assert(CFifoPeek(s_hUsbWork) == nullptr);

	// A queued core event must survive stop/reinit without running while
	// USB is down (UsbProcess would reconnect a cable that is still present).
	UsbProcessQue(0);
	const unsigned starts = s_StartCount;
	HciAppUsbRelease(&App);
	assert(!App.UsbRunning);
	assert(s_StartCount == starts);
	assert(HciAppUsbSetup(&App, HCI_USB_DESCRIPTOR_CDC_H4));
	assert(UsbEnable(0));
	HciAppUsbWorkExec();
	const unsigned afterRestart = s_ProcessCount;
	UsbProcessQue(0);
	HciAppUsbWorkExec();
	assert(s_ProcessCount == afterRestart + 1U); // No stranded queued latch.
	assert(!AppEvtHandlerPending());
	HciAppUsbRelease(&App);
	assert(HciAppUsbSetup(&App, HCI_USB_DESCRIPTOR_LOG_ONLY));
	assert(s_LogCdc.FirstInterface() == 0U);
	s_Vbus = false;
	HciAppStartLogPort(&App);
	assert(App.UsbRunning);
	HciAppUsbWorkExec();
	const unsigned beforeAttach = s_StartCount;
	s_Vbus = true;
	UsbProcessQue(0);
	HciAppUsbWorkExec();
	assert(s_StartCount == beforeAttach + 1U);
	HciAppUsbRelease(&App);
}

int main(void)
{
	static HciApp_t app;
	s_pApp = &app;
	assert(HciAppUsbSetup(&app, HCI_USB_DESCRIPTOR_NATIVE_HCI));
	app.Initialized = true;
	BtHciUsb &hci = s_HciUsb;
	UsbdCdc &log = s_LogCdc;

	assert(hci.FirstInterface() == 0U);
	assert(hci.InterfaceCount() == 2U);
	assert(hci.EpInMask() == ((1U << 1) | (1U << 2)));
	assert(hci.EpOutMask() == (1U << 2));
	assert(log.FirstInterface() == 2U);
	assert(log.InterfaceCount() == 2U);
	assert(log.EpInMask() == ((1U << 3) | (1U << 4)));
	assert(log.EpOutMask() == (1U << 4));

	uint16_t length = 0U;
	const uint8_t *p = UsbGetDescriptor(0, USB_DESCTYPE_DEVICE, 0U, 0U,
		USB_SPEED_FULL, &length);
	assert(p != nullptr && length == sizeof(UsbDevDesc_t));
	const UsbDevDesc_t *pDevice = reinterpret_cast<const UsbDevDesc_t *>(p);
	assert(pDevice->idVendor == HciUsbDescriptorVid());
	assert(pDevice->idProduct ==
		HciUsbDescriptorPid(HCI_USB_DESCRIPTOR_NATIVE_HCI));
	assert(pDevice->bDeviceClass == USB_DEVCLASS_MISC);
	assert(pDevice->bDeviceSubClass == 2U);
	assert(pDevice->bDeviceProtocol == 1U);

	p = UsbGetDescriptor(0, USB_DESCTYPE_CONFIGURATION, 0U, 0U,
		USB_SPEED_FULL, &length);
	assert(p != nullptr);
	assert(length == sizeof(UsbCfgDesc_t) + sizeof(BtHciUsbSerialDesc_t) +
		sizeof(UsbdCdcDesc_t));
	const UsbCfgDesc_t *pConfig = reinterpret_cast<const UsbCfgDesc_t *>(p);
	assert(pConfig->wTotalLength == length);
	assert(pConfig->bNumInterfaces == 4U);
	assert((pConfig->bmAttributes & USB_CONFATT_REMOTE_WAKEUP) == 0U);

	const size_t hciOffset = sizeof(UsbCfgDesc_t);
	const BtHciUsbSerialDesc_t *pHci =
		reinterpret_cast<const BtHciUsbSerialDesc_t *>(&p[hciOffset]);
	CheckHci(pHci);

	const size_t logOffset = hciOffset + sizeof(BtHciUsbSerialDesc_t);
	const UsbdCdcDesc_t *pLog =
		reinterpret_cast<const UsbdCdcDesc_t *>(&p[logOffset]);
	assert(pLog->Association.bFirstInterface == 2U);
	assert(pLog->Control.bInterfaceNumber == 2U);
	assert(pLog->Data.bInterfaceNumber == 3U);
	assert(pLog->Notification.bEndpointAddress == USB_ENDPADDR_DIRIN(3U));
	assert(pLog->Out.bEndpointAddress == USB_ENDPADDR_DIROUT(4U));
	assert(pLog->In.bEndpointAddress == USB_ENDPADDR_DIRIN(4U));

	assert(s_CtrlrInitialized);
	assert(s_RegisteredEpCount >= 6);
	assert(UsbEnable(0));

	assert(s_ProcessCount == 0U);
	assert(s_WorkWakeCount != 0U);
	HciAppUsbWorkExec();
	assert(s_ProcessCount == 1U);
	assert(!AppEvtHandlerPending());
	CheckWorkerQueue(app);
	printf("hci_usb_test: pass\n");
	return 0;
}


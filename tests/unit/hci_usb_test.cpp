/* Native Bluetooth HCI composite topology over IOsonata main USB core.
 * LE-only controller: no SCO alternates. */

#include "hci_usb.h"
#include "usb/usbd_cdc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int s_RegisteredEpCount;
static bool s_CtrlrInitialized;

extern "C" {
bool UsbCtrlrInit(int DevNo, const UsbCtrlrCfg_t *pCfg)
{
	if (DevNo != 0 || pCfg == nullptr)
	{
		return false;
	}
	s_CtrlrInitialized = true;
	return true;
}

bool UsbCtrlrStart(int DevNo) { return DevNo == 0; }
void UsbCtrlrStop(int) {}
void UsbCtrlrProcess(int) {}
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

int main(void)
{
	alignas(4) uint8_t hciRx[BT_HCI_USB_ACL_RXMEM_SIZE(8U)];
	alignas(4) uint8_t hciTx[BT_HCI_USB_ACL_TXMEM_SIZE(20U)];
	alignas(4) uint8_t logRx[USB_INTRF_RXMEM_SIZE(8U, USB_CTRLR_PKT_LEN_MAX(0, BULK))];
	alignas(4) uint8_t logTx[CFIFO_MEMSIZE(4096U)];

	UsbCfg_t usbCfg = {};
	usbCfg.DevNo = 0;
	usbCfg.Mode = USB_MODE_DEVICE;
	usbCfg.Vid = HciUsbDescriptorVid();
	usbCfg.Pid = HciUsbDescriptorPid(HCI_USB_DESCRIPTOR_NATIVE_HCI);
	usbCfg.DevVer = 0x0100U;
	usbCfg.pManufacturer = "I-SYST inc.";
	usbCfg.pProduct = "I-SYST HCI Controller";
	usbCfg.pSerial = nullptr;
	usbCfg.pFuncName = "HCI Controller";
	usbCfg.IntPrio = 7;
	usbCfg.DeviceClass = USB_DEVCLASS_MISC;
	usbCfg.DeviceSubClass = 2U;
	usbCfg.DeviceProtocol = 1U;
	usbCfg.bSelfPowered = false;
	usbCfg.bRemoteWakeup = false;
	usbCfg.bLowPowerSuspend = false;
	usbCfg.MaxPower = 100U;
	assert(UsbInit(&usbCfg));

	BtHciUsb hci;
	BtHciUsbCfg_t hciCfg = {};
	hciCfg.DevNo = 0;
	hciCfg.bBlocking = true;
	hciCfg.bSco = false;
	hciCfg.bBulkSerialization = true;
	hciCfg.RxFifoMemSize = sizeof(hciRx);
	hciCfg.pRxFifoMem = hciRx;
	hciCfg.TxFifoMemSize = sizeof(hciTx);
	hciCfg.pTxFifoMem = hciTx;
	hciCfg.InterfaceString = HCI_USB_STRING_FUNCTION;
	assert(hci.Init(hciCfg));

	UsbdCdc log;
	UsbdCdcCfg_t logCfg = {};
	logCfg.DevNo = 0;
	logCfg.bBlocking = true;
	logCfg.RxFifoMemSize = sizeof(logRx);
	logCfg.pRxFifoMem = logRx;
	logCfg.TxFifoMemSize = sizeof(logTx);
	logCfg.pTxFifoMem = logTx;
	assert(log.Init(logCfg));

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

	printf("hci_usb_test: pass\n");
	return 0;
}

/* Native Bluetooth HCI composite topology over IOsonata main USB core. */

#include "hci_usb.h"
#include "usb/usbd_cdc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static UsbCtrlrEvtHandler_t s_CoreHandler;
static void *s_CoreContext;
static int s_RegisteredEpCount;

extern "C" {
bool UsbCtrlrInit(int DevNo, const UsbCtrlrCfg_t *pCfg)
{
	if (DevNo != 0 || pCfg == nullptr || pCfg->EvtHandler == nullptr)
	{
		return false;
	}
	s_CoreHandler = pCfg->EvtHandler;
	s_CoreContext = pCfg->pContext;
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
void UsbCtrlrEpClose(int, uint8_t) {}
void UsbCtrlrEpCloseAll(int) {}

bool UsbCtrlrEpRegister(int DevNo, uint8_t, uint8_t *pBuffer, bool,
						UsbCtrlrEpHandler_t Handler, void *)
{
	if (DevNo != 0 || pBuffer == nullptr || Handler == nullptr)
	{
		return false;
	}
	s_RegisteredEpCount++;
	return true;
}

bool UsbCtrlrEpXfer(int DevNo, uint8_t, uint16_t) { return DevNo == 0; }
bool UsbCtrlrEp0Xfer(int DevNo, uint8_t, uint8_t *, uint16_t)
{
	return DevNo == 0;
}
void UsbCtrlrEpStall(int, uint8_t) {}
void UsbCtrlrEpClearStall(int, uint8_t) {}
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

static void CheckFullHci(const BtHciUsbFullDesc_t *pHci)
{
	static const uint16_t scoMps[BT_HCI_USB_SCO_ALT_COUNT] = {
		9U, 17U, 25U, 33U, 49U, 63U,
	};

	assert(pHci != nullptr);
	assert(pHci->Base.Association.bFirstInterface == 0U);
	assert(pHci->Base.Association.bInterfaceCount == 2U);
	assert(pHci->Base.Hci.bInterfaceNumber == 0U);
	assert(pHci->Base.Hci.bAlternateSetting == 0U);
	assert(pHci->Base.EventIn.bEndpointAddress == USB_ENDPADDR_DIRIN(1U));
	assert(pHci->Base.AclOut.bEndpointAddress == USB_ENDPADDR_DIROUT(2U));
	assert(pHci->Base.AclIn.bEndpointAddress == USB_ENDPADDR_DIRIN(2U));
	assert(pHci->Base.Serialized.Interface.bInterfaceNumber == 0U);
	assert(pHci->Base.Serialized.Interface.bAlternateSetting == 1U);
	assert(pHci->Base.Serialized.Interface.bNumEndpoints == 2U);
	assert(pHci->Base.Serialized.Out.bEndpointAddress ==
		pHci->Base.AclOut.bEndpointAddress);
	assert(pHci->Base.Serialized.In.bEndpointAddress ==
		pHci->Base.AclIn.bEndpointAddress);
	assert(pHci->Base.Sync.bInterfaceNumber == 1U);
	assert(pHci->Base.Sync.bAlternateSetting == 0U);
	assert(pHci->Base.Sync.bNumEndpoints == 0U);

	for (uint8_t i = 0U; i < BT_HCI_USB_SCO_ALT_COUNT; i++)
	{
		const BtHciUsbScoAltDesc_t *pAlt = &pHci->Alt[i];
		assert(pAlt->Interface.bInterfaceNumber == 1U);
		assert(pAlt->Interface.bAlternateSetting == (uint8_t)(i + 1U));
		assert(pAlt->Interface.bNumEndpoints == 2U);
		assert(pAlt->Out.bEndpointAddress == USB_ENDPADDR_DIROUT(8U));
		assert(pAlt->In.bEndpointAddress == USB_ENDPADDR_DIRIN(8U));
		assert((pAlt->Out.bmAttributes & USB_ENDPATT_TRANS_MASK) ==
			USB_ENDPATT_TRANS_ISO);
		assert((pAlt->In.bmAttributes & USB_ENDPATT_TRANS_MASK) ==
			USB_ENDPATT_TRANS_ISO);
		assert(pAlt->Out.wMaxPacketSize == scoMps[i]);
		assert(pAlt->In.wMaxPacketSize == scoMps[i]);
	}
}

int main(void)
{
	alignas(4) uint8_t hciRx[BT_HCI_USB_ACL_RXMEM_SIZE(8U)];
	alignas(4) uint8_t hciTx[BT_HCI_USB_ACL_TXMEM_SIZE(20U)];
	alignas(4) uint8_t logRx[USB_INTRF_RXMEM_SIZE(8U, USB_PKT_MAXLEN(0, BULK))];
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
	hciCfg.bSco = true;
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
	assert(hci.EpInMask() == ((1U << 1) | (1U << 2) | (1U << 8)));
	assert(hci.EpOutMask() == ((1U << 2) | (1U << 8)));
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
	assert(length == sizeof(UsbCfgDesc_t) + sizeof(BtHciUsbFullDesc_t) +
		sizeof(UsbdCdcDesc_t));
	const UsbCfgDesc_t *pConfig = reinterpret_cast<const UsbCfgDesc_t *>(p);
	assert(pConfig->wTotalLength == length);
	assert(pConfig->bNumInterfaces == 4U);
	assert((pConfig->bmAttributes & USB_CONFATT_REMOTE_WAKEUP) == 0U);

	const size_t hciOffset = sizeof(UsbCfgDesc_t);
	const BtHciUsbFullDesc_t *pHci =
		reinterpret_cast<const BtHciUsbFullDesc_t *>(&p[hciOffset]);
	CheckFullHci(pHci);

	const size_t logOffset = hciOffset + sizeof(BtHciUsbFullDesc_t);
	const UsbdCdcDesc_t *pLog =
		reinterpret_cast<const UsbdCdcDesc_t *>(&p[logOffset]);
	assert(pLog->Association.bFirstInterface == 2U);
	assert(pLog->Control.bInterfaceNumber == 2U);
	assert(pLog->Data.bInterfaceNumber == 3U);
	assert(pLog->Notification.bEndpointAddress == USB_ENDPADDR_DIRIN(3U));
	assert(pLog->Out.bEndpointAddress == USB_ENDPADDR_DIROUT(4U));
	assert(pLog->In.bEndpointAddress == USB_ENDPADDR_DIRIN(4U));

	assert(s_CoreHandler != nullptr);
	assert(s_CoreContext == nullptr);
	assert(s_RegisteredEpCount >= 8);
	assert(UsbEnable(0));

	printf("hci_usb_test: pass\n");
	return 0;
}

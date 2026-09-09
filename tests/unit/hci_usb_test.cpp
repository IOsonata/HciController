/* Native Bluetooth HCI composite topology over IOsonata BtHciUsb. */

#include "hci_usb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static UsbCfg_t s_UsbCfg;
static UsbFuncCfg_t s_Function;
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
void UsbCtrlrEpClose(int, uint8_t) {}
void UsbCtrlrEpCloseAll(int) {}

bool UsbCtrlrEpRegister(int, uint8_t, uint8_t *pBuffer, bool,
						UsbCtrlrEpHandler_t Handler, void *)
{
	return pBuffer != nullptr && Handler != nullptr;
}

bool UsbCtrlrEpXfer(int, uint8_t, uint16_t) { return true; }
bool UsbCtrlrEp0Xfer(int, uint8_t, uint8_t *, uint16_t) { return true; }
void UsbCtrlrEpStall(int, uint8_t) {}
void UsbCtrlrEpClearStall(int, uint8_t) {}
size_t UsbCtrlrGetSerial(int, char *pBuff, size_t BuffLen)
{
	if (pBuff != nullptr && BuffLen != 0U)
	{
		pBuff[0] = 0;
	}
	return 0U;
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
	s_UsbCfg.DevNo = 0;
	s_UsbCfg.Vid = HciUsbDescriptorVid();
	s_UsbCfg.Pid = HciUsbDescriptorPid(HCI_USB_DESCRIPTOR_NATIVE_HCI);
	s_UsbCfg.DevVer = 0x0100U;
	s_UsbCfg.pManufacturer = "I-SYST inc.";
	s_UsbCfg.pProduct = "HciController";
	s_UsbCfg.pSerial = "01234567";
	s_Configured = true;
}

static void CheckFullHci(const BtHciUsbFullDesc_t *pHci)
{
	static const uint16_t scoMps[BT_HCI_USB_SCO_ALT_COUNT] = {
		9U, 17U, 25U, 33U, 49U, 63U,
	};

	assert(pHci->Base.Association.bFirstInterface == 0U);
	assert(pHci->Base.Association.bInterfaceCount == 2U);
	assert(pHci->Base.Hci.bInterfaceNumber == 0U);
	assert(pHci->Base.Hci.bAlternateSetting == 0U);
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
		assert((pAlt->Out.bmAttributes & 0x03U) == USB_ENDPATT_TRANS_ISO);
		assert((pAlt->In.bmAttributes & 0x03U) == USB_ENDPATT_TRANS_ISO);
		assert(pAlt->Out.wMaxPacketSize == scoMps[i]);
		assert(pAlt->In.wMaxPacketSize == scoMps[i]);
	}
}

static void CheckComposite(const BtHciUsbFullDesc_t *pHci)
{
	assert(HciUsbDescriptorSetMode(HCI_USB_DESCRIPTOR_NATIVE_HCI));
	assert(HciUsbDescriptorSetFullHci(pHci));

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
	assert(p != nullptr);
	assert(length == 9U + sizeof(*pHci) + 66U);
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

	alignas(4) uint8_t rxMem[BT_HCI_USB_ACL_RXMEM_SIZE(8U)];
	alignas(4) uint8_t txMem[BT_HCI_USB_ACL_TXMEM_SIZE(20U)];
	BtHciUsbFullDesc_t hciDesc = {};
	BtHciUsbCfg_t cfg = {};
	cfg.bBlocking = true;
	cfg.bSco = true;
	cfg.bBulkSerialization = true;
	cfg.RxFifoMemSize = sizeof(rxMem);
	cfg.pRxFifoMem = rxMem;
	cfg.TxFifoMemSize = sizeof(txMem);
	cfg.pTxFifoMem = txMem;
	cfg.DevNo = 0;
	cfg.InterfaceString = HCI_USB_STRING_BT;
	cfg.pFullDesc = &hciDesc;

	BtHciUsb usb;
	assert(usb.Init(cfg));
	assert(s_Function.FirstInterface == 0U);
	assert(s_Function.InterfaceCount == 2U);
	assert(s_Function.EpInMask == ((1U << 1) | (1U << 2) | (1U << 8)));
	assert(s_Function.EpOutMask == ((1U << 2) | (1U << 8)));

	CheckFullHci(&hciDesc);
	CheckComposite(&hciDesc);

	BtHciUsbFullDesc_t bad = hciDesc;
	bad.Alt[0].Out.wMaxPacketSize = HCI_USB_SCO_MAX_MPS + 1U;
	assert(!HciUsbDescriptorSetFullHci(&bad));

	printf("hci_usb_test: pass\n");
	return 0;
}

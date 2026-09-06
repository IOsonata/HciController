/**-------------------------------------------------------------------------
@file	hci_usb.cpp

@brief	Native Bluetooth HCI class for the IOsonata USB stack.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#include "hci_usb.h"

#include <string.h>

#include "coredev/interrupt.h"

#define HCI_USB_CLASS_WIRELESS_CONTROLLER	0xE0U
#define HCI_USB_SUBCLASS_RF_CONTROLLER		0x01U
#define HCI_USB_PROTOCOL_BLUETOOTH			0x01U
#define HCI_USB_CONFIG_VALUE					1U
#define HCI_USB_COMMAND_HEADER_SIZE			3U
#define HCI_USB_EVENT_HEADER_SIZE			2U
#define HCI_USB_ACL_HEADER_SIZE				4U
#define HCI_USB_SCO_HEADER_SIZE				3U
#define HCI_USB_ISO_HEADER_SIZE				4U
#define HCI_USB_HISTORICAL_COMMAND_REQUEST	0xE0U

typedef struct {
	uint32_t Sequence;
	uint16_t Length;
	uint8_t EventCode;
	uint8_t Valid;
	uint32_t Crc32;
} HciUsbTxValidationRecord_t;

#ifndef HCI_USB_TX_VALIDATION_DEPTH
#define HCI_USB_TX_VALIDATION_DEPTH 8U
#endif

static HciUsb *s_pUsb;
static HciUsbTxValidationRecord_t s_TxValidation[HCI_USB_TX_VALIDATION_DEPTH];
static uint32_t s_TxValidationSequence;
static uint32_t s_TxValidationWrite;

static uint16_t HciUsbReadLe16(const uint8_t *pData)
{
	return (uint16_t)pData[0] | ((uint16_t)pData[1] << 8);
}

static void HciUsbWriteLe16(uint8_t *pData, uint16_t Value)
{
	pData[0] = (uint8_t)Value;
	pData[1] = (uint8_t)(Value >> 8);
}

static void HciUsbWriteLe32(uint8_t *pData, uint32_t Value)
{
	pData[0] = (uint8_t)Value;
	pData[1] = (uint8_t)(Value >> 8);
	pData[2] = (uint8_t)(Value >> 16);
	pData[3] = (uint8_t)(Value >> 24);
}

static uint32_t HciUsbCrc32(const uint8_t *pData, size_t Length)
{
	uint32_t crc = 0xFFFFFFFFU;

	for (size_t i = 0U; i < Length; i++)
	{
		crc ^= pData[i];
		for (unsigned bit = 0U; bit < 8U; bit++)
		{
			crc = (crc >> 1) ^ (0xEDB88320U &
				(uint32_t)-(int32_t)(crc & 1U));
		}
	}

	return ~crc;
}

static bool HciUsbEventCodeValid(uint8_t Code)
{
	return Code != 0U && (Code <= 0x3EU || Code == 0xFFU);
}

static void HciUsbResetTxValidation(void)
{
	memset(s_TxValidation, 0, sizeof(s_TxValidation));
	s_TxValidationSequence = 0U;
	s_TxValidationWrite = 0U;
}

static void HciUsbRecordTxValidation(const uint8_t *pData, size_t Length)
{
	if (pData == nullptr || Length == 0U || Length > UINT16_MAX)
	{
		return;
	}

	HciUsbTxValidationRecord_t *pRecord =
		&s_TxValidation[s_TxValidationWrite % HCI_USB_TX_VALIDATION_DEPTH];
	pRecord->Sequence = ++s_TxValidationSequence;
	pRecord->Length = (uint16_t)Length;
	pRecord->EventCode = pData[0];
	pRecord->Valid = HciUsbEventCodeValid(pData[0]) ? 1U : 0U;
	pRecord->Crc32 = HciUsbCrc32(pData, Length);
	s_TxValidationWrite++;
}

extern "C" size_t HciUsbPlatformReadTxValidation(uint8_t *pData,
											 size_t Capacity)
{
	const size_t recordSize = 12U;
	const size_t headerSize = 8U;
	if (pData == nullptr || Capacity < headerSize)
	{
		return headerSize;
	}

	const uint32_t count = s_TxValidationWrite < HCI_USB_TX_VALIDATION_DEPTH ?
		s_TxValidationWrite : HCI_USB_TX_VALIDATION_DEPTH;
	const size_t required = headerSize + (size_t)count * recordSize;
	if (Capacity < required)
	{
		return required;
	}

	HciUsbWriteLe32(&pData[0], s_TxValidationSequence);
	HciUsbWriteLe32(&pData[4], count);
	const uint32_t first = s_TxValidationWrite > count ?
		s_TxValidationWrite - count : 0U;
	for (uint32_t i = 0U; i < count; i++)
	{
		const HciUsbTxValidationRecord_t *pRecord =
			&s_TxValidation[(first + i) % HCI_USB_TX_VALIDATION_DEPTH];
		const size_t offset = headerSize + (size_t)i * recordSize;
		HciUsbWriteLe32(&pData[offset], pRecord->Sequence);
		HciUsbWriteLe16(&pData[offset + 4U], pRecord->Length);
		pData[offset + 6U] = pRecord->EventCode;
		pData[offset + 7U] = pRecord->Valid;
		HciUsbWriteLe32(&pData[offset + 8U], pRecord->Crc32);
	}

	return required;
}

static size_t HciUsbPacketLength(HciH4PacketType_t Type,
							 const uint8_t *pPacket, size_t Available)
{
	if (pPacket == nullptr)
	{
		return 0U;
	}

	switch (Type)
	{
		case HCI_H4_PACKET_COMMAND:
			return Available >= HCI_USB_COMMAND_HEADER_SIZE ?
				HCI_USB_COMMAND_HEADER_SIZE + (size_t)pPacket[2] : 0U;

		case HCI_H4_PACKET_EVENT:
			return Available >= HCI_USB_EVENT_HEADER_SIZE ?
				HCI_USB_EVENT_HEADER_SIZE + (size_t)pPacket[1] : 0U;

		case HCI_H4_PACKET_ACL:
			return Available >= HCI_USB_ACL_HEADER_SIZE ?
				HCI_USB_ACL_HEADER_SIZE +
				(size_t)HciUsbReadLe16(&pPacket[2]) : 0U;

		case HCI_H4_PACKET_SCO:
			return Available >= HCI_USB_SCO_HEADER_SIZE ?
				HCI_USB_SCO_HEADER_SIZE + (size_t)pPacket[2] : 0U;

		case HCI_H4_PACKET_ISO:
			return Available >= HCI_USB_ISO_HEADER_SIZE ?
				HCI_USB_ISO_HEADER_SIZE +
				(size_t)(HciUsbReadLe16(&pPacket[2]) & 0x3FFFU) : 0U;

		default:
			return 0U;
	}
}

static size_t HciUsbHeaderSize(HciH4PacketType_t Type)
{
	switch (Type)
	{
		case HCI_H4_PACKET_COMMAND:
			return HCI_USB_COMMAND_HEADER_SIZE;
		case HCI_H4_PACKET_EVENT:
			return HCI_USB_EVENT_HEADER_SIZE;
		case HCI_H4_PACKET_ACL:
			return HCI_USB_ACL_HEADER_SIZE;
		case HCI_H4_PACKET_SCO:
			return HCI_USB_SCO_HEADER_SIZE;
		case HCI_H4_PACKET_ISO:
			return HCI_USB_ISO_HEADER_SIZE;
		default:
			return 0U;
	}
}

static bool HciUsbHostTypeValid(HciH4PacketType_t Type)
{
	return Type == HCI_H4_PACKET_COMMAND || Type == HCI_H4_PACKET_ACL ||
		Type == HCI_H4_PACKET_SCO || Type == HCI_H4_PACKET_ISO;
}

static bool HciUsbOutputTypeValid(HciH4PacketType_t Type)
{
	return Type == HCI_H4_PACKET_EVENT || Type == HCI_H4_PACKET_ACL ||
		Type == HCI_H4_PACKET_SCO || Type == HCI_H4_PACKET_ISO;
}

static bool HciUsbOpenEndpoint(int DevNo, uint8_t EpAddr, uint8_t Type,
							   uint16_t Mps, uint8_t Interval)
{
	UsbEndPointDesc_t desc = {};
	desc.bLength = sizeof(desc);
	desc.bDescriptorType = USB_DESCTYPE_ENDPOINT;
	desc.bEndpointAddress = EpAddr;
	desc.bmAttributes = Type;
	desc.wMaxPacketSize = Mps;
	desc.bInterval = Interval;
	return UsbCtrlrEpOpen(DevNo, &desc);
}

bool HciUsb::Init(const HciUsbCfg_t &Cfg)
{
	if ((s_pUsb != nullptr && s_pUsb != this) || Cfg.pRxFifoMem == nullptr ||
		Cfg.pTxFifoMem == nullptr || Cfg.RxFifoMemSize <= 0 ||
		Cfg.TxFifoMemSize <= 0 || UsbGetCfg(Cfg.DevNo) == nullptr)
	{
		return false;
	}

	vEvtCB = Cfg.EvtCB;
	vDevNo = Cfg.DevNo;
	ClearRx();
	ClearEventTx();
	vConfigured = false;
	vHciAlt = HCI_USB_HCI_ALT_LEGACY;
	vBulkSerialization = false;
	vCommandCount = 0U;
	vAclOutCount = 0U;
	vAclInCount = 0U;
	vEventInCount = 0U;
	vInvalidRxCount = 0U;
	vTxErrorCount = 0U;
	HciUsbResetTxValidation();
	s_pUsb = this;

	UsbIntrfCfg_t dataCfg = {};
	dataCfg.DevNo = Cfg.DevNo;
	dataCfg.EpNo = HCI_USB_BULK_EP_NO;
	dataCfg.bBlocking = Cfg.bBlocking;
	dataCfg.RxFifoMemSize = Cfg.RxFifoMemSize;
	dataCfg.pRxFifoMem = Cfg.pRxFifoMem;
	dataCfg.TxFifoMemSize = Cfg.TxFifoMemSize;
	dataCfg.pTxFifoMem = Cfg.pTxFifoMem;
	dataCfg.TxFifoBlkSize = HCI_USB_PKT_BLKSIZE;
	dataCfg.BufferSize = sizeof(vBulkRxTransfer);
	dataCfg.pRxBuffer = vBulkRxTransfer;
	dataCfg.pTxBuffer = vBulkTxTransfer;
	dataCfg.EvtCB = DataEventHandler;
	if (!UsbIntrf::Init(dataCfg))
	{
		s_pUsb = nullptr;
		return false;
	}

	vBulkRxData = vUsbDevIntrf.DevIntrf.RxData;
	vBulkTxData = vUsbDevIntrf.DevIntrf.TxData;
	vUsbDevIntrf.DevIntrf.StartRx = DevStartRx;
	vUsbDevIntrf.DevIntrf.RxData = DevRxData;
	vUsbDevIntrf.DevIntrf.StartTx = DevStartTx;
	vUsbDevIntrf.DevIntrf.TxData = DevTxData;
	vUsbDevIntrf.DevIntrf.TxSrData = DevTxSrData;
	vUsbDevIntrf.DevIntrf.Reset = DevReset;
	vUsbDevIntrf.DevIntrf.GetHandle = DevGetHandle;

	if (!UsbCtrlrEpRegister(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_EVENT_EP_NO),
		vEventTxTransfer, EventXferHandler, this))
	{
		s_pUsb = nullptr;
		return false;
	}

	UsbFuncCfg_t cfg = {};
	cfg.FirstInterface = 0U;
	cfg.InterfaceCount = 2U;
	cfg.EpInMask = (uint16_t)((1U << HCI_USB_EVENT_EP_NO) |
		(1U << HCI_USB_BULK_EP_NO));
	cfg.EpOutMask = (uint16_t)(1U << HCI_USB_BULK_EP_NO);
	cfg.RequestHandler = RequestHandler;
	cfg.ConfigHandler = ConfigHandler;
	cfg.SetInterfaceHandler = SetInterfaceHandler;
	cfg.XferHandler = XferHandler;
	cfg.ResetHandler = ResetHandler;
	cfg.ProcessHandler = ProcessHandler;
	cfg.pContext = this;
	if (!UsbRegisterFunc(vDevNo, &cfg))
	{
		s_pUsb = nullptr;
		return false;
	}

	return true;
}

bool HciUsb::Open(uint8_t Alt)
{
	if (Alt > HCI_USB_HCI_ALT_SERIALIZED)
	{
		return false;
	}

	if (Alt == HCI_USB_HCI_ALT_LEGACY &&
		!HciUsbOpenEndpoint(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_EVENT_EP_NO),
			USB_ENDPATT_TRANS_INT, HCI_USB_EVENT_MPS, 1U))
	{
		return false;
	}

	if (!HciUsbOpenEndpoint(vDevNo, USB_ENDPADDR_DIROUT(HCI_USB_BULK_EP_NO),
			USB_ENDPATT_TRANS_BULK, HCI_USB_FS_BULK_MPS, 0U) ||
		!HciUsbOpenEndpoint(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_BULK_EP_NO),
			USB_ENDPATT_TRANS_BULK, HCI_USB_FS_BULK_MPS, 0U) ||
		!UsbIntrfConfigure(&vUsbDevIntrf, HCI_USB_FS_BULK_MPS))
	{
		UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_EVENT_EP_NO));
		UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIROUT(HCI_USB_BULK_EP_NO));
		UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_BULK_EP_NO));
		UsbIntrfUnconfigure(&vUsbDevIntrf);
		return false;
	}

	vHciAlt = Alt;
	vBulkSerialization = Alt == HCI_USB_HCI_ALT_SERIALIZED;
	vConfigured = true;
	return true;
}

void HciUsb::Close(void)
{
	// Stop rearming before an endpoint close can complete a cancelled transfer.
	vConfigured = false;
	UsbIntrfUnconfigure(&vUsbDevIntrf);
	UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_EVENT_EP_NO));
	UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIROUT(HCI_USB_BULK_EP_NO));
	UsbCtrlrEpClose(vDevNo, USB_ENDPADDR_DIRIN(HCI_USB_BULK_EP_NO));
	ClearRx();
	ClearEventTx();
}

bool HciUsb::ConfigHandler(uint8_t Configuration, void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb == nullptr)
	{
		return false;
	}

	pUsb->Close();
	pUsb->vHciAlt = HCI_USB_HCI_ALT_LEGACY;
	pUsb->vBulkSerialization = false;
	return Configuration == 0U ||
		(Configuration == HCI_USB_CONFIG_VALUE &&
		 pUsb->Open(HCI_USB_HCI_ALT_LEGACY));
}

bool HciUsb::SetInterfaceHandler(uint8_t InterfaceNo, uint8_t Alt,
									 void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb == nullptr || !pUsb->vConfigured)
	{
		return false;
	}
	if (InterfaceNo == 1U)
	{
		return Alt == 0U;
	}
	if (InterfaceNo != 0U || Alt > HCI_USB_HCI_ALT_SERIALIZED)
	{
		return false;
	}

	const uint8_t oldAlt = pUsb->vHciAlt;
	pUsb->Close();
	if (pUsb->Open(Alt))
	{
		return true;
	}

	(void)pUsb->Open(oldAlt);
	return false;
}

bool HciUsb::RequestHandler(const UsbSetupData_t *pSetup,
							 UsbCtrlStage_t Stage, uint8_t **ppData,
							 uint16_t *pLength, void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb == nullptr || pSetup == nullptr || pLength == nullptr ||
		!pUsb->vConfigured || pUsb->vBulkSerialization ||
		(pSetup->bmRequestType & USB_REQTYPE_MASK_DIR) != USB_REQTYPE_DIRDEV ||
		(pSetup->bmRequestType & USB_REQTYPE_MASK_TYPE) != USB_REQTYPE_CLASS ||
		(pSetup->bRequest != 0U &&
		 pSetup->bRequest != HCI_USB_HISTORICAL_COMMAND_REQUEST) ||
		pSetup->wLength < HCI_USB_COMMAND_HEADER_SIZE ||
		pSetup->wLength > sizeof(pUsb->vCommandBuffer))
	{
		return false;
	}

	const uint8_t recipient =
		pSetup->bmRequestType & USB_REQTYPE_MASK_RECIPIENT;
	if (recipient == USB_REQTYPE_INTERFACE)
	{
		if (pSetup->wIndex != 0U)
		{
			return false;
		}
	}
	else if (recipient != USB_REQTYPE_DEVICE)
	{
		return false;
	}

	if (Stage == USB_CTRL_SETUP)
	{
		if (ppData == nullptr || pUsb->vCommandPending)
		{
			return false;
		}
		*ppData = pUsb->vCommandBuffer;
		*pLength = pSetup->wLength;
		return true;
	}
	if (Stage == USB_CTRL_DATA)
	{
		const size_t packetLen = HciUsbPacketLength(HCI_H4_PACKET_COMMAND,
			pUsb->vCommandBuffer, *pLength);
		return *pLength == pSetup->wLength && packetLen == *pLength;
	}
	if (Stage == USB_CTRL_COMPLETE)
	{
		pUsb->vCommandLen = pSetup->wLength;
		pUsb->vCommandPending = true;
		pUsb->Wake(DEVINTRF_EVT_RX_DATA, (int)pUsb->vCommandLen);
		return true;
	}
	if (Stage == USB_CTRL_ABORT)
	{
		pUsb->vCommandLen = 0U;
		return true;
	}

	return false;
}

void HciUsb::XferHandler(uint8_t EpAddr, uint16_t Length,
							 UsbCtrlrXferResult_t Result, void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb != nullptr && USB_ENDPADDR_NUM(EpAddr) == HCI_USB_BULK_EP_NO)
	{
		UsbIntrfXferComplete(&pUsb->vUsbDevIntrf, EpAddr, Length, Result);
	}
}

void HciUsb::ResetHandler(void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb != nullptr)
	{
		pUsb->Close();
		pUsb->vHciAlt = HCI_USB_HCI_ALT_LEGACY;
		pUsb->vBulkSerialization = false;
	}
}

void HciUsb::ProcessHandler(void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb != nullptr)
	{
		pUsb->Process();
	}
}

void HciUsb::EventXferHandler(uint8_t, uint16_t Length,
								  UsbCtrlrXferResult_t Result, void *pContext)
{
	HciUsb *pUsb = static_cast<HciUsb *>(pContext);
	if (pUsb == nullptr || !pUsb->vEventTxActive)
	{
		return;
	}

	const bool wasZlp = pUsb->vEventTxZlp;
	const uint16_t expected = wasZlp ? 0U : pUsb->vEventTxChunkLen;
	pUsb->vEventTxActive = false;
	pUsb->vEventTxZlp = false;
	if (Result != USB_CTRLR_XFER_SUCCESS || Length != expected)
	{
		pUsb->vTxErrorCount++;
		pUsb->ClearEventTx();
		pUsb->Wake(DEVINTRF_EVT_TX_TIMEOUT, Length);
		return;
	}

	if (!wasZlp)
	{
		pUsb->vEventTxOffset += pUsb->vEventTxChunkLen;
		pUsb->vEventTxChunkLen = 0U;
		if (pUsb->vEventTxOffset < pUsb->vEventTxLen)
		{
			(void)pUsb->SendEventPacket();
			return;
		}
	}

	if (pUsb->vEventTxNeedZlp && !wasZlp)
	{
		(void)pUsb->SendEventZlp();
		return;
	}

	pUsb->ClearEventTx();
	pUsb->Wake(DEVINTRF_EVT_TX_READY, 0);
}

int HciUsb::DataEventHandler(DevIntrf_t * const, DEVINTRF_EVT Evt,
								 uint8_t *pBuffer, int Length)
{
	if (s_pUsb == nullptr)
	{
		return Length;
	}

	if (Evt == DEVINTRF_EVT_RX_DATA)
	{
		// UsbIntrf has already copied the DMA buffer into its packet CFifo.
		// Assembly remains in application context; this callback only wakes it.
		s_pUsb->Wake(Evt, Length);
		return Length;
	}

	if (s_pUsb->vEvtCB != nullptr)
	{
		return s_pUsb->vEvtCB(s_pUsb->Data(), Evt, pBuffer, Length);
	}
	return Length;
}

bool HciUsb::DevStartRx(DevIntrf_t * const pDev, uint32_t DevAddr)
{
	if (s_pUsb == nullptr || pDev != s_pUsb->Data())
	{
		return false;
	}
	s_pUsb->vRxSelect = static_cast<HciH4PacketType_t>(DevAddr);
	return s_pUsb->vRxSelect == HCI_H4_PACKET_COMMAND ||
		s_pUsb->vRxSelect == HCI_H4_PACKET_ACL ||
		s_pUsb->vRxSelect == HCI_H4_PACKET_SCO ||
		s_pUsb->vRxSelect == HCI_H4_PACKET_ISO;
}

int HciUsb::DevRxData(DevIntrf_t * const pDev, uint8_t *pBuffer,
						int BufferLen)
{
	return s_pUsb != nullptr && pDev == s_pUsb->Data() ?
		s_pUsb->Receive(pBuffer, BufferLen) : 0;
}

bool HciUsb::DevStartTx(DevIntrf_t * const pDev, uint32_t DevAddr)
{
	if (s_pUsb == nullptr || pDev != s_pUsb->Data() ||
		!s_pUsb->vConfigured)
	{
		return false;
	}
	s_pUsb->vTxSelect = static_cast<HciH4PacketType_t>(DevAddr);
	return HciUsbOutputTypeValid(s_pUsb->vTxSelect);
}

int HciUsb::DevTxData(DevIntrf_t * const pDev, const uint8_t *pData,
						int DataLen)
{
	return s_pUsb != nullptr && pDev == s_pUsb->Data() ?
		s_pUsb->Transmit(pData, DataLen) : 0;
}

int HciUsb::DevTxSrData(DevIntrf_t * const pDev, const uint8_t *pData,
						  int DataLen)
{
	return DevTxData(pDev, pData, DataLen);
}

void HciUsb::DevReset(DevIntrf_t * const pDev)
{
	if (s_pUsb != nullptr && pDev == s_pUsb->Data())
	{
		s_pUsb->ClearRx();
		UsbIntrfUnconfigure(&s_pUsb->vUsbDevIntrf);
	}
}

void *HciUsb::DevGetHandle(DevIntrf_t * const pDev)
{
	return s_pUsb != nullptr && pDev == s_pUsb->Data() ? s_pUsb : nullptr;
}

void HciUsb::ClearRx(void)
{
	vRxSelect = HCI_H4_PACKET_NONE;
	vBulkRxType = HCI_H4_PACKET_NONE;
	vCommandPending = false;
	vBulkRxPending = false;
	vCommandLen = 0U;
	vBulkRxLen = 0U;
	vBulkRxExpected = 0U;
}

void HciUsb::ClearEventTx(void)
{
	vEventTxActive = false;
	vEventTxNeedZlp = false;
	vEventTxZlp = false;
	vEventTxLen = 0U;
	vEventTxOffset = 0U;
	vEventTxChunkLen = 0U;
}

void HciUsb::Wake(DEVINTRF_EVT Evt, int Length)
{
	if (vEvtCB != nullptr)
	{
		(void)vEvtCB(Data(), Evt, nullptr, Length);
	}
}

bool HciUsb::CompleteBulkPacket(void)
{
	if (vBulkRxLen == 0U)
	{
		return false;
	}

	size_t offset = 0U;
	HciH4PacketType_t type = HCI_H4_PACKET_ACL;
	if (vBulkSerialization)
	{
		type = static_cast<HciH4PacketType_t>(vBulkRxBuffer[0]);
		if (!HciUsbHostTypeValid(type))
		{
			vInvalidRxCount++;
			vBulkRxLen = 0U;
			return false;
		}
		offset = 1U;
	}

	const size_t headerSize = HciUsbHeaderSize(type);
	if (headerSize == 0U || vBulkRxLen < offset + headerSize)
	{
		return false;
	}

	const size_t payloadLength = HciUsbPacketLength(type,
		&vBulkRxBuffer[offset], vBulkRxLen - offset);
	if (payloadLength == 0U || payloadLength > HCI_USB_PACKET_SIZE)
	{
		vInvalidRxCount++;
		vBulkRxLen = 0U;
		return false;
	}

	vBulkRxExpected = offset + payloadLength;
	if (vBulkRxLen < vBulkRxExpected)
	{
		return false;
	}
	if (vBulkRxLen != vBulkRxExpected)
	{
		vInvalidRxCount++;
		vBulkRxLen = 0U;
		vBulkRxExpected = 0U;
		return false;
	}

	vBulkRxType = type;
	vBulkRxPending = true;
	Wake(DEVINTRF_EVT_RX_DATA, (int)payloadLength);
	return true;
}

bool HciUsb::ConsumePhysicalPacket(void)
{
	UsbPkt_t *pPacket = reinterpret_cast<UsbPkt_t *>(
		CFifoPeek(vUsbDevIntrf.hRxFifo));
	if (pPacket == nullptr)
	{
		return false;
	}

	const uint16_t length = pPacket->Hdr.Length;
	if (length == 0U)
	{
		uint32_t state = DisableInterrupt();
		(void)CFifoGet(vUsbDevIntrf.hRxFifo);
		if (CFifoAvail(vUsbDevIntrf.hRxFifo) > 0)
		{
			(void)UsbCtrlrEpRxArm(vDevNo, HCI_USB_BULK_EP_NO);
		}
		EnableInterrupt(state);
		if (vBulkRxLen != 0U)
		{
			vInvalidRxCount++;
			vBulkRxLen = 0U;
			vBulkRxExpected = 0U;
		}
		return true;
	}

	if (length > HCI_USB_FS_BULK_MPS ||
		vBulkRxLen + length > sizeof(vBulkRxBuffer))
	{
		uint32_t state = DisableInterrupt();
		(void)CFifoGet(vUsbDevIntrf.hRxFifo);
		if (CFifoAvail(vUsbDevIntrf.hRxFifo) > 0)
		{
			(void)UsbCtrlrEpRxArm(vDevNo, HCI_USB_BULK_EP_NO);
		}
		EnableInterrupt(state);
		vInvalidRxCount++;
		vBulkRxLen = 0U;
		vBulkRxExpected = 0U;
		return true;
	}

	const int count = vBulkRxData(&vUsbDevIntrf.DevIntrf,
		&vBulkRxBuffer[vBulkRxLen], length);
	if (count != length)
	{
		vInvalidRxCount++;
		vBulkRxLen = 0U;
		vBulkRxExpected = 0U;
		return count > 0;
	}

	vBulkRxLen += length;
	if (CompleteBulkPacket())
	{
		return true;
	}
	if (vBulkRxLen != 0U && length < HCI_USB_FS_BULK_MPS)
	{
		vInvalidRxCount++;
		vBulkRxLen = 0U;
		vBulkRxExpected = 0U;
	}
	return true;
}

void HciUsb::ConsumeBulkRx(void)
{
	while (vConfigured && !vBulkRxPending && ConsumePhysicalPacket())
	{
	}
}

void HciUsb::Process(void)
{
	if (!vConfigured)
	{
		return;
	}
	ConsumeBulkRx();
	if (vEventTxLen != 0U && !vEventTxActive)
	{
		if (vEventTxOffset < vEventTxLen)
		{
			(void)SendEventPacket();
		}
		else if (vEventTxNeedZlp)
		{
			(void)SendEventZlp();
		}
	}
}

int HciUsb::Receive(uint8_t *pBuffer, int BufferLen)
{
	if (pBuffer == nullptr || BufferLen <= 0)
	{
		return 0;
	}

	if (vRxSelect == HCI_H4_PACKET_COMMAND && vCommandPending)
	{
		if ((size_t)BufferLen < vCommandLen)
		{
			return 0;
		}
		memcpy(pBuffer, vCommandBuffer, vCommandLen);
		const int count = (int)vCommandLen;
		vCommandPending = false;
		vCommandLen = 0U;
		vCommandCount++;
		return count;
	}

	if (!vBulkRxPending || vRxSelect != vBulkRxType ||
		(size_t)BufferLen < vBulkRxExpected - (vBulkSerialization ? 1U : 0U))
	{
		return 0;
	}

	const size_t offset = vBulkSerialization ? 1U : 0U;
	const size_t count = vBulkRxExpected - offset;
	memcpy(pBuffer, &vBulkRxBuffer[offset], count);
	if (vBulkRxType == HCI_H4_PACKET_ACL)
	{
		vAclOutCount++;
	}
	vBulkRxPending = false;
	vBulkRxType = HCI_H4_PACKET_NONE;
	vBulkRxLen = 0U;
	vBulkRxExpected = 0U;
	ConsumeBulkRx();
	return (int)count;
}

int HciUsb::QueueBulk(HciH4PacketType_t Type, const uint8_t *pData,
					 size_t DataLen)
{
	const size_t wireLength = DataLen + (vBulkSerialization ? 1U : 0U);
	const size_t packetCount = (wireLength + HCI_USB_FS_BULK_MPS - 1U) /
		HCI_USB_FS_BULK_MPS;
	const bool needZlp = wireLength != 0U &&
		(wireLength % HCI_USB_FS_BULK_MPS) == 0U;
	const size_t blocks = packetCount + (needZlp ? 1U : 0U);
	if (blocks == 0U || blocks > INT32_MAX ||
		!UsbIntrfRequestToSend(&vUsbDevIntrf,
			(int)(blocks * HCI_USB_PKT_BLKSIZE)))
	{
		return 0;
	}

	alignas(4) uint8_t storage[HCI_USB_PKT_BLKSIZE] = {};
	UsbPkt_t *pPacket = reinterpret_cast<UsbPkt_t *>(storage);
	size_t wireOffset = 0U;
	for (size_t packet = 0U; packet < packetCount; packet++)
	{
		const size_t remaining = wireLength - wireOffset;
		const size_t length = remaining < HCI_USB_FS_BULK_MPS ?
			remaining : HCI_USB_FS_BULK_MPS;
		pPacket->Hdr.Length = (uint16_t)length;
		pPacket->Hdr.Reserved = 0U;
		for (size_t i = 0U; i < length; i++, wireOffset++)
		{
			pPacket->Data[i] = vBulkSerialization && wireOffset == 0U ?
				(uint8_t)Type : pData[wireOffset -
					(vBulkSerialization ? 1U : 0U)];
		}
		if (vBulkTxData(&vUsbDevIntrf.DevIntrf, storage,
			(int)sizeof(storage)) != (int)sizeof(storage))
		{
			vTxErrorCount++;
			return 0;
		}
	}

	if (needZlp)
	{
		pPacket->Hdr.Length = 0U;
		pPacket->Hdr.Reserved = 0U;
		if (vBulkTxData(&vUsbDevIntrf.DevIntrf, storage,
			(int)sizeof(storage)) != (int)sizeof(storage))
		{
			vTxErrorCount++;
			return 0;
		}
	}

	if (Type == HCI_H4_PACKET_ACL)
	{
		vAclInCount++;
	}
	else if (Type == HCI_H4_PACKET_EVENT)
	{
		vEventInCount++;
	}
	return (int)DataLen;
}

bool HciUsb::SendEventPacket(void)
{
	if (vEventTxActive || vEventTxOffset >= vEventTxLen)
	{
		return false;
	}

	const size_t remaining = vEventTxLen - vEventTxOffset;
	vEventTxChunkLen = (uint16_t)(remaining < HCI_USB_EVENT_MPS ?
		remaining : HCI_USB_EVENT_MPS);
	memcpy(vEventTxTransfer, &vEventTxBuffer[vEventTxOffset],
		vEventTxChunkLen);
	vEventTxActive = true;
	vEventTxZlp = false;
	if (!UsbCtrlrEpSend(vDevNo, HCI_USB_EVENT_EP_NO, vEventTxChunkLen))
	{
		vEventTxActive = false;
		vEventTxChunkLen = 0U;
		return false;
	}
	return true;
}

int HciUsb::SendEvent(const uint8_t *pData, size_t DataLen)
{
	if (vEventTxActive || vEventTxLen != 0U ||
		DataLen > sizeof(vEventTxBuffer))
	{
		return 0;
	}
	memcpy(vEventTxBuffer, pData, DataLen);
	vEventTxLen = DataLen;
	vEventTxOffset = 0U;
	vEventTxNeedZlp = (DataLen % HCI_USB_EVENT_MPS) == 0U;
	if (!SendEventPacket())
	{
		ClearEventTx();
		vTxErrorCount++;
		return 0;
	}
	HciUsbRecordTxValidation(pData, DataLen);
	vEventInCount++;
	return (int)DataLen;
}

bool HciUsb::SendEventZlp(void)
{
	if (!vEventTxNeedZlp || vEventTxActive)
	{
		return false;
	}
	vEventTxActive = true;
	vEventTxZlp = true;
	vEventTxChunkLen = 0U;
	if (!UsbCtrlrEpSend(vDevNo, HCI_USB_EVENT_EP_NO, 0U))
	{
		vEventTxActive = false;
		vEventTxZlp = false;
		return false;
	}
	return true;
}

int HciUsb::Transmit(const uint8_t *pData, int DataLen)
{
	if (pData == nullptr || DataLen <= 0 || !vConfigured ||
		!HciUsbOutputTypeValid(vTxSelect) ||
		HciUsbPacketLength(vTxSelect, pData, (size_t)DataLen) !=
			(size_t)DataLen)
	{
		return 0;
	}

	if (vBulkSerialization)
	{
		return QueueBulk(vTxSelect, pData, (size_t)DataLen);
	}
	if (vTxSelect == HCI_H4_PACKET_EVENT)
	{
		return SendEvent(pData, (size_t)DataLen);
	}
	if (vTxSelect == HCI_H4_PACKET_ACL)
	{
		return QueueBulk(vTxSelect, pData, (size_t)DataLen);
	}

	return 0;
}

bool HciUsb::IsOpen(void) const
{
	return vConfigured && UsbConfigured(vDevNo);
}

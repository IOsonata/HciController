/**-------------------------------------------------------------------------
@file	hci_usb.h

@brief	Native Bluetooth USB HCI transport.

		HciUsb is the Bluetooth class adapter for the IOsonata USB stack.
		Endpoint zero receives HCI commands, interrupt IN endpoint 1 sends
		events, and the inherited UsbIntrf endpoint pair 2 carries ACL data.

@author	Nguyen Hoan Hoang
@date	September 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_USB_H
#define HCI_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_intrf.h"
#include "hci_h4.h"
#include "usb/usb.h"

#define HCI_USB_PACKET_SIZE			1024U
#define HCI_USB_COMMAND_SIZE			258U
#define HCI_USB_FS_BULK_MPS			64U
#define HCI_USB_EVENT_MPS			16U
#define HCI_USB_EVENT_EP_NO			1U
#define HCI_USB_BULK_EP_NO			2U
#define HCI_USB_HCI_ALT_LEGACY		0U
#define HCI_USB_HCI_ALT_SERIALIZED	1U

#define HCI_USB_HCI_TRANSPORT_CDC_H4	1
#define HCI_USB_HCI_TRANSPORT_NATIVE	2

#ifndef HCI_USB_HCI_TRANSPORT
#define HCI_USB_HCI_TRANSPORT HCI_USB_HCI_TRANSPORT_NATIVE
#endif

typedef enum {
	HCI_USB_DESCRIPTOR_LOG_ONLY = 0,
	HCI_USB_DESCRIPTOR_CDC_H4 = 1,
	HCI_USB_DESCRIPTOR_NATIVE_HCI = 2,
} HciUsbDescriptorMode_t;

#ifdef __cplusplus
extern "C" {
#endif

bool HciUsbDescriptorSetMode(HciUsbDescriptorMode_t Mode);
uint16_t HciUsbDescriptorVid(void);
uint16_t HciUsbDescriptorPid(HciUsbDescriptorMode_t Mode);

const uint8_t *HciUsbDescHandler(uint8_t DescType, uint8_t DescIndex,
								 uint16_t LangId, UsbSpeed_t Speed,
								 uint16_t *pLength, void *pContext);

#ifdef __cplusplus
}

#include "usb/usb_intrf.h"

#define HCI_USB_PKT_BLKSIZE	USB_INTRF_PKT_BLKSIZE(HCI_USB_FS_BULK_MPS)

typedef struct {
	bool bBlocking;
	int RxFifoMemSize;
	uint8_t *pRxFifoMem;
	int TxFifoMemSize;
	uint8_t *pTxFifoMem;
	int DevNo;
	DevIntrfEvtHandler_t EvtCB;
} HciUsbCfg_t;

class HciUsb : public UsbIntrf {
public:
	HciUsb() = default;

	bool Init(const HciUsbCfg_t &Cfg);
	void Process(void);
	bool IsOpen(void) const;
	bool BulkSerialization(void) const { return vBulkSerialization; }
	DevIntrf_t *Data(void) { return static_cast<DevIntrf_t *>(*this); }

	uint32_t CommandCount(void) const { return vCommandCount; }
	uint32_t AclOutCount(void) const { return vAclOutCount; }
	uint32_t AclInCount(void) const { return vAclInCount; }
	uint32_t EventInCount(void) const { return vEventInCount; }
	uint32_t InvalidRxCount(void) const { return vInvalidRxCount; }
	uint32_t TxErrorCount(void) const { return vTxErrorCount; }

private:
	typedef int (*DataFct_t)(DevIntrf_t * const, uint8_t *, int);
	typedef int (*TxFct_t)(DevIntrf_t * const, const uint8_t *, int);

	static bool ConfigHandler(uint8_t Configuration, void *pContext);
	static bool SetInterfaceHandler(uint8_t InterfaceNo, uint8_t Alt,
									void *pContext);
	static bool RequestHandler(const UsbSetupData_t *pSetup,
							  UsbCtrlStage_t Stage, uint8_t **ppData,
							  uint16_t *pLength, void *pContext);
	static void XferHandler(uint8_t EpAddr, uint16_t Length,
							UsbCtrlrXferResult_t Result, void *pContext);
	static void ResetHandler(void *pContext);
	static void ProcessHandler(void *pContext);
	static void EventXferHandler(uint8_t EpAddr, uint16_t Length,
								 UsbCtrlrXferResult_t Result, void *pContext);
	static int DataEventHandler(DevIntrf_t * const pDev, DEVINTRF_EVT Evt,
								uint8_t *pBuffer, int Length);

	static bool DevStartRx(DevIntrf_t * const pDev, uint32_t DevAddr);
	static int DevRxData(DevIntrf_t * const pDev, uint8_t *pBuffer,
						 int BufferLen);
	static bool DevStartTx(DevIntrf_t * const pDev, uint32_t DevAddr);
	static int DevTxData(DevIntrf_t * const pDev, const uint8_t *pData,
						 int DataLen);
	static int DevTxSrData(DevIntrf_t * const pDev, const uint8_t *pData,
						   int DataLen);
	static void DevReset(DevIntrf_t * const pDev);
	static void *DevGetHandle(DevIntrf_t * const pDev);

	bool Open(uint8_t Alt);
	void Close(void);
	void ClearRx(void);
	void ClearEventTx(void);
	void Wake(DEVINTRF_EVT Evt, int Length);
	void ConsumeBulkRx(void);
	bool ConsumePhysicalPacket(void);
	bool CompleteBulkPacket(void);
	int Receive(uint8_t *pBuffer, int BufferLen);
	int Transmit(const uint8_t *pData, int DataLen);
	int QueueBulk(HciH4PacketType_t Type, const uint8_t *pData,
				  size_t DataLen);
	int SendEvent(const uint8_t *pData, size_t DataLen);
	bool SendEventPacket(void);
	bool SendEventZlp(void);

	DevIntrfEvtHandler_t vEvtCB = nullptr;
	DataFct_t vBulkRxData = nullptr;
	TxFct_t vBulkTxData = nullptr;
	int vDevNo = 0;
	HciH4PacketType_t vRxSelect = HCI_H4_PACKET_NONE;
	HciH4PacketType_t vTxSelect = HCI_H4_PACKET_NONE;
	HciH4PacketType_t vBulkRxType = HCI_H4_PACKET_NONE;
	uint8_t vHciAlt = HCI_USB_HCI_ALT_LEGACY;
	bool vConfigured = false;
	bool vBulkSerialization = false;
	bool vCommandPending = false;
	bool vBulkRxPending = false;
	bool vEventTxActive = false;
	bool vEventTxNeedZlp = false;
	bool vEventTxZlp = false;
	size_t vCommandLen = 0U;
	size_t vBulkRxLen = 0U;
	size_t vBulkRxExpected = 0U;
	size_t vEventTxLen = 0U;
	size_t vEventTxOffset = 0U;
	uint16_t vEventTxChunkLen = 0U;
	uint32_t vCommandCount = 0U;
	uint32_t vAclOutCount = 0U;
	uint32_t vAclInCount = 0U;
	uint32_t vEventInCount = 0U;
	uint32_t vInvalidRxCount = 0U;
	uint32_t vTxErrorCount = 0U;
	alignas(4) uint8_t vCommandBuffer[HCI_USB_COMMAND_SIZE] = {};
	alignas(4) uint8_t vBulkRxBuffer[HCI_USB_PACKET_SIZE + 1U] = {};
	alignas(4) uint8_t vEventTxBuffer[HCI_USB_PACKET_SIZE] = {};
	alignas(4) uint8_t vEventTxTransfer[HCI_USB_EVENT_MPS] = {};
	alignas(4) uint8_t vBulkRxTransfer[HCI_USB_FS_BULK_MPS] = {};
	alignas(4) uint8_t vBulkTxTransfer[HCI_USB_FS_BULK_MPS] = {};
};

#endif

#endif /* HCI_USB_H */
/*
 * Regression: a defined H:4 packet type that is invalid Host->Controller must
 * still be consumed from the packet DeviceIntrf and rejected. Otherwise a
 * pending Event packet can block the interface forever before the next command.
 */

#include "hci_controller.h"

#include <assert.h>
#include <string.h>

struct FakePacketIntrf
{
    DevIntrf_t Dev;
    bool Pending;
    HciH4PacketType_t Type;
    uint8_t Packet[16];
    int Len;
};

static FakePacketIntrf s_Host;
static unsigned s_PutCount;
static HciH4PacketType_t s_LastPutType;

int DeviceIntrfRx(DevIntrf_t * const pDev,
                  uint32_t DevAddr,
                  uint8_t *pBuffer,
                  int BufferLen)
{
    FakePacketIntrf *pHost = static_cast<FakePacketIntrf *>(pDev->pDevData);
    if (!pHost->Pending || DevAddr != (uint32_t)pHost->Type ||
        BufferLen < pHost->Len)
    {
        return 0;
    }

    memcpy(pBuffer, pHost->Packet, (size_t)pHost->Len);
    const int len = pHost->Len;
    pHost->Pending = false;
    return len;
}

int DeviceIntrfTx(DevIntrf_t * const,
                  uint32_t,
                  const uint8_t *,
                  int DataLen)
{
    return DataLen;
}

void DeviceIntrfEnable(DevIntrf_t *)
{
}

void DeviceIntrfDisable(DevIntrf_t *)
{
}

static bool Put(void *,
                HciH4PacketType_t Type,
                const uint8_t *,
                size_t)
{
    s_PutCount++;
    s_LastPutType = Type;
    return true;
}

static HciControllerGetResult_t Get(void *,
                                    HciH4PacketType_t *,
                                    uint8_t *,
                                    size_t,
                                    size_t *)
{
    return HCI_CONTROLLER_GET_EMPTY;
}

static void Queue(HciH4PacketType_t Type,
                  const uint8_t *pPacket,
                  size_t Len)
{
    assert(!s_Host.Pending && Len <= sizeof(s_Host.Packet));
    memcpy(s_Host.Packet, pPacket, Len);
    s_Host.Type = Type;
    s_Host.Len = (int)Len;
    s_Host.Pending = true;
}

int main(void)
{
    memset(&s_Host, 0, sizeof(s_Host));
    s_Host.Dev.pDevData = &s_Host;

    HciControllerOps_t ops = {};
    ops.Put = Put;
    ops.Get = Get;

    HciController_t controller = {};
    uint8_t hostPacket[64] = {};
    uint8_t controllerPacket[64] = {};
    assert(HciControllerInitPacketTransport(&controller,
                                            &s_Host.Dev,
                                            hostPacket,
                                            sizeof(hostPacket),
                                            controllerPacket,
                                            sizeof(controllerPacket),
                                            &ops));

    const uint8_t invalidEvent[] = {0x0EU, 0x00U};
    Queue(HCI_H4_PACKET_EVENT, invalidEvent, sizeof(invalidEvent));
    HciControllerProcess(&controller);
    assert(!s_Host.Pending);
    assert(!controller.HostPacketPending);
    assert(controller.InvalidHostPacketCount == 1U);
    assert(s_PutCount == 0U);

    const uint8_t reset[] = {0x03U, 0x0CU, 0x00U};
    Queue(HCI_H4_PACKET_COMMAND, reset, sizeof(reset));
    HciControllerProcess(&controller);
    assert(!s_Host.Pending);
    assert(s_PutCount == 1U);
    assert(s_LastPutType == HCI_H4_PACKET_COMMAND);

    return 0;
}

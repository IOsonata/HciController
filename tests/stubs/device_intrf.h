/* Host-build subset of IOsonata device_intrf.h. */
#ifndef STUB_DEVICE_INTRF_H
#define STUB_DEVICE_INTRF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
using namespace std;
#else
#include <stdatomic.h>
#endif

typedef enum {
    DEVINTRF_EVT_RX_TIMEOUT,
    DEVINTRF_EVT_RX_DATA,
    DEVINTRF_EVT_RX_FIFO_FULL,
    DEVINTRF_EVT_TX_TIMEOUT,
    DEVINTRF_EVT_TX_READY,
    DEVINTRF_EVT_TX_FIFO_EMPTY,
    DEVINTRF_EVT_STATECHG,
    DEVINTRF_EVT_READ_RQST,
    DEVINTRF_EVT_WRITE_RQST,
    DEVINTRF_EVT_COMPLETED,
} DEVINTRF_EVT;

typedef enum {
    DEVINTRF_TYPE_NULL,
    DEVINTRF_TYPE_UNKOWN,
    DEVINTRF_TYPE_BT,
    DEVINTRF_TYPE_ETH,
    DEVINTRF_TYPE_I2C,
    DEVINTRF_TYPE_CEL,
    DEVINTRF_TYPE_SPI,
    DEVINTRF_TYPE_QSPI,
    DEVINTRF_TYPE_UART,
    DEVINTRF_TYPE_USB,
    DEVINTRF_TYPE_WIFI,
    DEVINTRF_TYPE_I2S,
    DEVINTRF_TYPE_PDM,
    DEVINTRF_TYPE_OSPI,
    DEVINTRF_TYPE_I3C,
    DEVINTRF_TYPE_CRYPTO,
    DEVINTRF_TYPE_MEMCTRL,
} DEVINTRF_TYPE;

struct __device_intrf;
typedef struct __device_intrf DevIntrf_t;
typedef int (*DevIntrfEvtHandler_t)(DevIntrf_t * const pDev,
                                    DEVINTRF_EVT Evt,
                                    uint8_t *pBuffer,
                                    int BufferLen);

struct __device_intrf {
    void *pDevData;
    int IntPrio;
    DevIntrfEvtHandler_t EvtCB;
    atomic_flag bBusy;
    int MaxRetry;
    atomic_int EnCnt;
    DEVINTRF_TYPE Type;
    bool bDma;
    bool bIntEn;
    atomic_bool bTxReady;
    atomic_bool bNoStop;

    void (*Disable)(DevIntrf_t * const);
    void (*Enable)(DevIntrf_t * const);
    uint32_t (*GetRate)(DevIntrf_t * const);
    uint32_t (*SetRate)(DevIntrf_t * const, uint32_t);
    bool (*StartRx)(DevIntrf_t * const, uint32_t);
    int (*RxData)(DevIntrf_t * const, uint8_t *, int);
    void (*StopRx)(DevIntrf_t * const);
    bool (*StartTx)(DevIntrf_t * const, uint32_t);
    int (*TxData)(DevIntrf_t * const, const uint8_t *, int);
    int (*TxSrData)(DevIntrf_t * const, const uint8_t *, int);
    void (*StopTx)(DevIntrf_t * const);
    void (*Reset)(DevIntrf_t * const);
    void (*PowerOff)(DevIntrf_t * const);
    void *(*GetHandle)(DevIntrf_t * const);
};

static inline uint32_t DeviceIntrfGetRate(DevIntrf_t * const pDev)
{
    return pDev->GetRate(pDev);
}

static inline uint32_t DeviceIntrfSetRate(DevIntrf_t * const pDev, uint32_t Rate)
{
    return pDev->SetRate(pDev, Rate);
}

static inline bool DeviceIntrfStartRx(DevIntrf_t * const pDev, uint32_t DevAddr)
{
    if (atomic_flag_test_and_set(&pDev->bBusy))
    {
        return false;
    }
    const bool result = pDev->StartRx(pDev, DevAddr);
    if (!result)
    {
        atomic_flag_clear(&pDev->bBusy);
    }
    return result;
}

static inline int DeviceIntrfRxData(DevIntrf_t * const pDev,
                                    uint8_t *pBuff,
                                    int BuffLen)
{
    return pDev->RxData(pDev, pBuff, BuffLen);
}

static inline void DeviceIntrfStopRx(DevIntrf_t * const pDev)
{
    pDev->StopRx(pDev);
    atomic_flag_clear(&pDev->bBusy);
}

static inline bool DeviceIntrfStartTx(DevIntrf_t * const pDev, uint32_t DevAddr)
{
    if (atomic_flag_test_and_set(&pDev->bBusy))
    {
        return false;
    }
    const bool result = pDev->StartTx(pDev, DevAddr);
    if (!result)
    {
        atomic_flag_clear(&pDev->bBusy);
    }
    return result;
}

static inline int DeviceIntrfTxData(DevIntrf_t * const pDev,
                                    const uint8_t *pData,
                                    int DataLen)
{
    return pDev->TxData(pDev, pData, DataLen);
}

static inline int DeviceIntrfTxSrData(DevIntrf_t * const pDev,
                                      const uint8_t *pData,
                                      int DataLen)
{
    return pDev->TxSrData(pDev, pData, DataLen);
}

static inline void DeviceIntrfStopTx(DevIntrf_t * const pDev)
{
    pDev->StopTx(pDev);
    atomic_flag_clear(&pDev->bBusy);
}

#ifdef __cplusplus
extern "C" {
#endif

int DeviceIntrfRx(DevIntrf_t *, uint32_t, uint8_t *, int);
int DeviceIntrfTx(DevIntrf_t *, uint32_t, const uint8_t *, int);
void DeviceIntrfEnable(DevIntrf_t *);
void DeviceIntrfDisable(DevIntrf_t *);

#ifdef __cplusplus
}
#endif

#endif

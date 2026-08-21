#ifndef USBD_PVT_H
#define USBD_PVT_H

#include "tusb.h"

typedef struct
{
    const char *name;
    void (*init)(void);
    bool (*deinit)(void);
    void (*reset)(uint8_t RhPort);
    uint16_t (*open)(uint8_t RhPort,
                     const tusb_desc_interface_t *pInterface,
                     uint16_t MaxLen);
    bool (*control_xfer_cb)(uint8_t RhPort,
                            uint8_t Stage,
                            const tusb_control_request_t *pRequest);
    bool (*xfer_cb)(uint8_t RhPort,
                    uint8_t EpAddr,
                    xfer_result_t Result,
                    uint32_t Transferred);
    bool (*xfer_isr)(uint8_t RhPort,
                     uint8_t EpAddr,
                     xfer_result_t Result,
                     uint32_t Transferred);
    void (*sof)(uint8_t RhPort, uint32_t FrameCount);
} usbd_class_driver_t;

#ifdef __cplusplus
extern "C" {
#endif

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *pDriverCount);
bool usbd_edpt_open(uint8_t RhPort, const tusb_desc_endpoint_t *pEndpoint);
void usbd_edpt_close(uint8_t RhPort, uint8_t EpAddr);
bool usbd_edpt_xfer(uint8_t RhPort,
                    uint8_t EpAddr,
                    uint8_t *pBuffer,
                    uint16_t Len);
bool usbd_edpt_busy(uint8_t RhPort, uint8_t EpAddr);

#ifdef __cplusplus
}
#endif

#endif /* USBD_PVT_H */

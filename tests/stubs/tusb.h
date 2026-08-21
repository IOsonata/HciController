#ifndef STUB_TUSB_H
#define STUB_TUSB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tusb_config.h"

#define TUSB_VERSION_MAJOR       0
#define TUSB_VERSION_MINOR       19
#define TUSB_VERSION_REVISION    0
#define TUSB_VERSION_NUMBER \
    (TUSB_VERSION_MAJOR * 10000 + TUSB_VERSION_MINOR * 100 + TUSB_VERSION_REVISION)

#define TUSB_DESC_DEVICE                 1U
#define TUSB_DESC_CONFIGURATION          2U
#define TUSB_DESC_STRING                 3U
#define TUSB_DESC_INTERFACE              4U
#define TUSB_DESC_ENDPOINT               5U
#define TUSB_DESC_INTERFACE_ASSOCIATION 11U
#define TUSB_DESC_CS_INTERFACE          0x24U

#define TUSB_XFER_CONTROL        0U
#define TUSB_XFER_ISOCHRONOUS    1U
#define TUSB_XFER_BULK           2U
#define TUSB_XFER_INTERRUPT      3U
#define TUSB_DIR_OUT             0U
#define TUSB_DIR_IN              1U
#define TUSB_DIR_IN_MASK         0x80U
#define TUSB_REQ_TYPE_STANDARD   0U
#define TUSB_REQ_TYPE_CLASS      1U
#define TUSB_REQ_RCPT_DEVICE     0U
#define TUSB_REQ_RCPT_INTERFACE  1U
#define TUSB_REQ_GET_INTERFACE   10U
#define TUSB_REQ_SET_INTERFACE   11U
#define CONTROL_STAGE_SETUP      0U
#define CONTROL_STAGE_DATA       1U
#define CONTROL_STAGE_ACK        2U

#define TUSB_CLASS_MISC          0xEFU
#define MISC_SUBCLASS_COMMON     0x02U
#define MISC_PROTOCOL_IAD        0x01U

#define TUD_CONFIG_DESC_LEN      9U
#define TUD_CDC_DESC_LEN         66U

#define U16_TO_U8S_LE(Value) \
    (uint8_t)((uint16_t)(Value) & 0xFFU), \
    (uint8_t)(((uint16_t)(Value) >> 8) & 0xFFU)

#define TUD_CONFIG_DESCRIPTOR(Config, Interfaces, StringIndex, TotalLen, Attr, PowerMa) \
    9U, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(TotalLen), Interfaces, Config, \
    StringIndex, (uint8_t)(0x80U | (Attr)), (uint8_t)((PowerMa) / 2U)

/* Standard 66-byte TinyUSB CDC-ACM descriptor shape used by usb_descriptors.c. */
#define TUD_CDC_DESCRIPTOR(Itf, Str, NotifyEp, NotifySize, OutEp, InEp, EpSize) \
    8U, TUSB_DESC_INTERFACE_ASSOCIATION, Itf, 2U, 0x02U, 0x02U, 0x01U, 0U, \
    9U, TUSB_DESC_INTERFACE, Itf, 0U, 1U, 0x02U, 0x02U, 0x01U, Str, \
    5U, TUSB_DESC_CS_INTERFACE, 0x00U, 0x20U, 0x01U, \
    5U, TUSB_DESC_CS_INTERFACE, 0x01U, 0x00U, (uint8_t)((Itf) + 1U), \
    4U, TUSB_DESC_CS_INTERFACE, 0x02U, 0x02U, \
    5U, TUSB_DESC_CS_INTERFACE, 0x06U, Itf, (uint8_t)((Itf) + 1U), \
    7U, TUSB_DESC_ENDPOINT, NotifyEp, TUSB_XFER_INTERRUPT, \
        U16_TO_U8S_LE(NotifySize), 16U, \
    9U, TUSB_DESC_INTERFACE, (uint8_t)((Itf) + 1U), 0U, 2U, 0x0AU, 0U, 0U, 0U, \
    7U, TUSB_DESC_ENDPOINT, OutEp, TUSB_XFER_BULK, U16_TO_U8S_LE(EpSize), 0U, \
    7U, TUSB_DESC_ENDPOINT, InEp, TUSB_XFER_BULK, U16_TO_U8S_LE(EpSize), 0U

typedef enum
{
    TUSB_ROLE_INVALID = 0,
    TUSB_ROLE_DEVICE = 1,
    TUSB_ROLE_HOST = 2,
} tusb_role_t;

typedef enum
{
    TUSB_SPEED_FULL = 0,
    TUSB_SPEED_HIGH = 1,
    TUSB_SPEED_AUTO = 2,
} tusb_speed_t;

typedef enum
{
    XFER_RESULT_SUCCESS = 0,
    XFER_RESULT_FAILED = 1,
    XFER_RESULT_STALLED = 2,
} xfer_result_t;

typedef struct
{
    tusb_role_t role;
    tusb_speed_t speed;
} tusb_rhport_init_t;

typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} tusb_desc_device_t;

typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} tusb_desc_interface_t;

typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    union
    {
        uint8_t value;
        struct
        {
            uint8_t xfer : 2;
            uint8_t sync : 2;
            uint8_t usage : 2;
            uint8_t reserved : 2;
        };
    } bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} tusb_desc_endpoint_t;

typedef struct __attribute__((packed))
{
    union
    {
        uint8_t bmRequestType;
        struct
        {
            uint8_t recipient : 5;
            uint8_t type : 2;
            uint8_t direction : 1;
        } bmRequestType_bit;
    };
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} tusb_control_request_t;

static inline const void *tu_desc_next(const void *pDesc)
{
    return (const uint8_t *)pDesc + *(const uint8_t *)pDesc;
}

static inline uint8_t tu_edpt_dir(uint8_t EpAddr)
{
    return (EpAddr & TUSB_DIR_IN_MASK) != 0U ? TUSB_DIR_IN : TUSB_DIR_OUT;
}

static inline uint16_t tu_edpt_packet_size(const tusb_desc_endpoint_t *pEndpoint)
{
    return pEndpoint->wMaxPacketSize & 0x07FFU;
}

#ifdef __cplusplus
extern "C" {
#endif

void tusb_int_handler(uint8_t RhPort, bool bInIsr);
bool tusb_rhport_init(uint8_t RhPort, const tusb_rhport_init_t *pInit);
bool tusb_deinit(uint8_t RhPort);
bool tud_deinit(uint8_t RhPort);
bool tusb_inited(void);
void tud_task_ext(uint32_t TimeoutMs, bool bInIsr);
bool tud_mounted(void);
bool tud_control_xfer(uint8_t RhPort,
                      const tusb_control_request_t *pRequest,
                      void *pBuffer,
                      uint16_t Len);
bool tud_control_status(uint8_t RhPort, const tusb_control_request_t *pRequest);

uint8_t const *tud_descriptor_device_cb(void);
uint8_t const *tud_descriptor_configuration_cb(uint8_t Index);
uint16_t const *tud_descriptor_string_cb(uint8_t Index, uint16_t LanguageId);
void tud_umount_cb(void);
void tud_event_hook_cb(uint8_t RhPort, uint32_t EventId, bool bInIsr);

uint8_t tud_cdc_n_get_line_state(uint8_t Interface);
uint32_t tud_cdc_n_available(uint8_t Interface);
uint32_t tud_cdc_n_read(uint8_t Interface, void *pBuffer, uint32_t Len);
uint32_t tud_cdc_n_write_available(uint8_t Interface);
uint32_t tud_cdc_n_write(uint8_t Interface, const void *pBuffer, uint32_t Len);
uint32_t tud_cdc_n_write_flush(uint8_t Interface);
void tud_cdc_rx_cb(uint8_t Interface);
void tud_cdc_tx_complete_cb(uint8_t Interface);
void tud_cdc_line_state_cb(uint8_t Interface, bool Dtr, bool Rts);

#ifdef __cplusplus
}
#endif

#endif

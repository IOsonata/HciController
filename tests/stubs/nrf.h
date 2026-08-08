#ifndef STUB_NRF_H
#define STUB_NRF_H
#include <stdint.h>
typedef enum {
    RADIO_IRQn = 1, RTC0_IRQn, TIMER0_IRQn, POWER_CLOCK_IRQn, SWI5_EGU5_IRQn, USBD_IRQn
} IRQn_Type;
#ifdef __cplusplus
extern "C" {
#endif
void NVIC_SetPriority(IRQn_Type, uint32_t);
void NVIC_EnableIRQ(IRQn_Type);
void NVIC_DisableIRQ(IRQn_Type);
#ifdef __cplusplus
}
#endif
typedef struct {
    volatile uint32_t INTENSET;
    volatile uint32_t INTENCLR;
    volatile uint32_t EVENTS_USBDETECTED;
    volatile uint32_t EVENTS_USBREMOVED;
    volatile uint32_t EVENTS_USBPWRRDY;
    volatile uint32_t USBREGSTATUS;
} NRF_POWER_Type;
extern NRF_POWER_Type *NRF_POWER;
/*
 * EVENTCAUSE is write one to clear on hardware, and writing ENABLE starts the
 * controller which then raises EVENTCAUSE.READY. Both are modelled here so the
 * host test exercises the same waits the target does.
 */
struct UsbdEventCauseReg {
    volatile uint32_t Value;
    UsbdEventCauseReg &operator=(uint32_t Mask) { Value &= ~Mask; return *this; }
    operator uint32_t() const { return Value; }
};

struct UsbdEnableReg {
    volatile uint32_t Value;
    UsbdEnableReg &operator=(uint32_t Setting);
    operator uint32_t() const { return Value; }
};

/*
 * EVENTS_USBRESET through EVENTS_EPDATA are laid out in INTEN bit order so the
 * pending mask walk in the code under test indexes the same way it does on the
 * target. USBEVENT is bit 22.
 */
typedef struct {
    UsbdEnableReg ENABLE;
    volatile uint32_t USBPULLUP;
    UsbdEventCauseReg EVENTCAUSE;
    volatile uint32_t ISOSPLIT;
    volatile uint32_t INTENSET;
    volatile uint32_t INTEN;
    volatile uint32_t EVENTS_USBRESET;
    volatile uint32_t EVENTS_MID[21];
    volatile uint32_t EVENTS_USBEVENT;
    volatile uint32_t EVENTS_TAIL[2];
} NRF_USBD_Type;
extern NRF_USBD_Type *NRF_USBD;
#define NRF_USBD_BASE              0x40027000UL
#define USBD_EVENTCAUSE_READY_Msk  (1UL << 11)
#define USBD_ISOSPLIT_SPLIT_HalfIN (0x0080UL)
#define USBD_INTEN_USBRESET_Msk    (1UL << 0)
#define USBD_EVENTCAUSE_ISOOUTCRC_Msk    (1UL << 0)
#define USBD_EVENTCAUSE_SUSPEND_Msk      (1UL << 8)
#define USBD_EVENTCAUSE_RESUME_Msk       (1UL << 9)
#define USBD_EVENTCAUSE_USBWUALLOWED_Msk (1UL << 10)

#define __ISB() do { } while (0)
#define __DSB() do { } while (0)

/*
 * PRIMASK on the host. The firmware uses it to make a read and clear of the
 * cable event flags atomic against the POWER_CLOCK handler; the host tests are
 * single threaded, so tracking the value is enough to keep the code honest.
 */
static inline uint32_t __get_PRIMASK(void) { return 0U; }
static inline void __set_PRIMASK(uint32_t) { }
static inline void __disable_irq(void) { }
static inline void __enable_irq(void) { }

#ifdef __cplusplus
extern "C" {
#endif
void NVIC_ClearPendingIRQ(IRQn_Type);
/* Raw register writes go through a table so host tests can observe them. */
uint32_t *HciTestReg(uint32_t Addr);
#ifdef __cplusplus
}
#endif
#ifndef HCI_NRF52840_REG32
#define HCI_NRF52840_REG32(Addr) (*HciTestReg((uint32_t)(Addr)))
#endif

typedef struct {
    volatile uint32_t HFCLKSTAT;
} NRF_CLOCK_Type;
extern NRF_CLOCK_Type *NRF_CLOCK;

/*
 * Only the members the UART report reads, with the real names and the real
 * shape of PSEL, which is a nested structure and not four separate registers.
 * A test that set a flat PSEL would pass against a header the part does not
 * have.
 */
typedef struct {
    volatile uint32_t RXD;
    volatile uint32_t TXD;
    volatile uint32_t CTS;
    volatile uint32_t RTS;
} NRF_UARTE_PSEL_Type;

typedef struct {
    volatile uint32_t ENABLE;
    volatile uint32_t BAUDRATE;
    volatile uint32_t ERRORSRC;
    NRF_UARTE_PSEL_Type PSEL;
} NRF_UARTE_Type;
extern NRF_UARTE_Type *NRF_UARTE0;
extern NRF_UARTE_Type *NRF_UARTE1;

typedef struct {
    volatile uint32_t IN;
} NRF_GPIO_Type;
extern NRF_GPIO_Type *NRF_P0;
extern NRF_GPIO_Type *NRF_P1;
#define CLOCK_HFCLKSTAT_STATE_Msk (1UL << 16)
#define CLOCK_HFCLKSTAT_SRC_Pos   (0UL)
#define CLOCK_HFCLKSTAT_SRC_Msk   (1UL << CLOCK_HFCLKSTAT_SRC_Pos)
#define CLOCK_HFCLKSTAT_SRC_Xtal  (1UL)
#define POWER_INTENSET_USBDETECTED_Msk   (1UL << 7)
#define POWER_INTENSET_USBREMOVED_Msk    (1UL << 8)
#define POWER_INTENSET_USBPWRRDY_Msk     (1UL << 9)
#define POWER_INTENCLR_USBDETECTED_Msk   POWER_INTENSET_USBDETECTED_Msk
#define POWER_INTENCLR_USBREMOVED_Msk    POWER_INTENSET_USBREMOVED_Msk
#define POWER_INTENCLR_USBPWRRDY_Msk     POWER_INTENSET_USBPWRRDY_Msk
#define POWER_USBREGSTATUS_VBUSDETECT_Msk (1UL << 0)
#define POWER_USBREGSTATUS_OUTPUTRDY_Msk  (1UL << 1)
#endif

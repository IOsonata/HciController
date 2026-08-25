/**-------------------------------------------------------------------------
@file	tusb_config.h

@brief	TinyUSB compile-time configuration for HciController.

		Configures full-speed device operation, memory alignment, endpoint zero,
		and the CDC instances required by H:4, native HCI, and log-only modes.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error "Define CFG_TUSB_MCU=OPT_MCU_NRF5X in the firmware project"
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

/*
 * Keep two CDC instances compiled into TinyUSB because the compatibility
 * CDC/H:4 descriptor uses CDC0 for HCI and CDC1 for the log. Native Bluetooth
 * USB does not consume a CDC instance for HCI; in that descriptor CDC0 is the
 * log and the custom Bluetooth class owns the HCI endpoints. Log-only mode
 * likewise uses CDC0 only. Compiling two instances is therefore the superset
 * needed by all three runtime descriptor modes.
 */
#define CFG_TUD_CDC    2
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_NOTIFY     1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_RX_EPSIZE  64
#define CFG_TUD_CDC_TX_EPSIZE  64

#endif /* TUSB_CONFIG_H */

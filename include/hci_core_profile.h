/**-------------------------------------------------------------------------
@file	hci_core_profile.h

@brief	Bluetooth Core version constants and product profile selection.

		Defines the Bluetooth Core versions recognized by HciController and
		the product-facing Core version exported by the current controller
		profile.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_CORE_PROFILE_H
#define HCI_CORE_PROFILE_H

#define HCI_CORE_VERSION_5_4 0x0DU
#define HCI_CORE_VERSION_6_0 0x0EU
#define HCI_CORE_VERSION_6_1 0x0FU
#define HCI_CORE_VERSION_6_2 0x10U
#define HCI_CORE_VERSION_6_3 0x11U

/*
 * Release 1 follows the capability intersection of nRF52840 and the current
 * sdk-nrfxlib multirole SDC. That combination supports the Core 6.0 Extended
 * Feature Set and the Core 6.2 Frame Space Update and Shorter Connection
 * Intervals features. The SDC link layer itself is at Core 6.3 (LL version
 * 0x11), so the product-facing HCI/LMP version is Core 6.3.
 *
 * The Core 6.3 features are Channel Sounding only (Inline PCT Transfer,
 * PHY-specific RTT accuracy). sdk-nrfxlib has Channel Sounding for nRF54L and
 * not for nRF52 or nRF53, so only the nRF54LM20 project links
 * hci_sdc_feat_cs.cpp. The version is only ever lowered to this value, never
 * raised: an older SDC keeps reporting its own version.
 *
 * nRF54LM20 is a separate release profile and is audited independently.
 */
#ifndef HCI_CONTROLLER_TARGET_CORE_VERSION
#define HCI_CONTROLLER_TARGET_CORE_VERSION HCI_CORE_VERSION_6_3
#endif

#if HCI_CONTROLLER_TARGET_CORE_VERSION > 0xFFU
#error "HCI_CONTROLLER_TARGET_CORE_VERSION must fit in one octet"
#endif

#endif /* HCI_CORE_PROFILE_H */

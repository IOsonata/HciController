/**-------------------------------------------------------------------------
@file	hci_sdc_feat_extfeat.cpp

@brief	SoftDevice Controller extended feature set, frame space update and shorter connection intervals.

		A project links this file to turn the feature on. Without it the
		weak defaults in hci_sdc_resources.cpp leave the feature out, and the
		library code behind its sdc_support_ call stays out of the image.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_sdc_resources.h"
#include "hci_core_profile.h"

extern "C" bool HciSdcFeatExtSupport(bool bSubrating)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_0
    sdc_support_extended_feature_set_central();
    sdc_support_extended_feature_set_peripheral();
#endif

#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_2
    /*
     * Both Core 6.2 additions depend on the extended feature set above, and
     * Shorter Connection Intervals also depends on connection subrating, so
     * the call follows only when that feature is linked too.
     */
    sdc_support_frame_space_update_central();
    sdc_support_frame_space_update_peripheral();
    if (bSubrating)
    {
        sdc_support_shorter_connection_intervals_central();
        sdc_support_shorter_connection_intervals_peripheral();
    }
#else
    (void)bSubrating;
#endif

    return true;
}

extern "C" bool HciSdcFeatExtConfig(void)
{
#if HCI_CONTROLLER_TARGET_CORE_VERSION >= HCI_CORE_VERSION_6_0
    /*
     * How many feature pages the controller keeps per link. The pool in
     * hci_sdc_resources.h is computed from the same macro.
     */
    sdc_cfg_t cfg = {};

    cfg.extended_feature_page_count = HCI_SDC_EXTENDED_FEATURE_PAGES;
    return HciSdcCfgSet(SDC_CFG_TYPE_EXTENDED_FEATURE_PAGE_COUNT, &cfg);
#else
    return true;
#endif
}

/**-------------------------------------------------------------------------
@file	hci_sdc_feat_past.cpp

@brief	SoftDevice Controller periodic advertising sync transfer.

		A project links this file to turn the feature on. Without it the
		weak defaults in hci_sdc_resources.cpp leave the feature out, and the
		library code behind its sdc_support_ call stays out of the image.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_sdc_resources.h"
#include "hci_core_profile.h"

extern "C" bool HciSdcFeatPastSupport(void)
{
    /*
     * Four calls rather than two. Sending and receiving a sync are separate
     * capabilities and each is per role.
     */
    sdc_support_periodic_adv_sync_transfer_sender_central();
    sdc_support_periodic_adv_sync_transfer_sender_peripheral();
    sdc_support_periodic_adv_sync_transfer_receiver_central();
    sdc_support_periodic_adv_sync_transfer_receiver_peripheral();
    return true;
}

/**-------------------------------------------------------------------------
@file	hci_sdc_feat_subrating.cpp

@brief	SoftDevice Controller connection subrating.

		A project links this file to turn the feature on. Without it the
		weak defaults in hci_sdc_resources.cpp leave the feature out, and the
		library code behind its sdc_support_ call stays out of the image.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_sdc_resources.h"
#include "hci_core_profile.h"

extern "C" bool HciSdcFeatSubratingSupport(void)
{
    sdc_support_connection_subrating_central();
    sdc_support_connection_subrating_peripheral();
    return true;
}

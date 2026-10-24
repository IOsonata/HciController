/**-------------------------------------------------------------------------
@file	hci_sdc_feat_pawr.cpp

@brief	SoftDevice Controller periodic advertising with responses.

		A project links this file to turn the feature on. Without it the
		weak defaults in hci_sdc_resources.cpp leave the feature out, and the
		library code behind its sdc_support_ call stays out of the image.

@author	Nguyen Hoan Hoang
@date	October 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_sdc_resources.h"
#include "hci_core_profile.h"

extern "C" bool HciSdcFeatPawrSupport(bool bPast)
{
    /* Needs a sync transfer sender or receiver. */
    if (!bPast)
    {
        return false;
    }

    sdc_support_le_periodic_adv_with_rsp();
    sdc_support_le_periodic_sync_with_rsp();
    return true;
}

extern "C" bool HciSdcFeatPawrConfig(void)
{
    sdc_cfg_t cfg = {};

    cfg.periodic_adv_rsp_count.count = HCI_SDC_PERIODIC_ADV_RSP_COUNT;
    if (!HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_RSP_COUNT, &cfg))
    {
        return false;
    }

    /*
     * All three buffer numbers together, because the vendor structure holds
     * them in one member and the pool macro takes all three.
     */
    cfg = {};
    cfg.periodic_adv_rsp_buffer_cfg.tx_buffer_count =
        HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.rx_buffer_count =
        HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS;
    cfg.periodic_adv_rsp_buffer_cfg.max_tx_data_size =
        HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA;
    return HciSdcCfgSet(SDC_CFG_TYPE_PERIODIC_ADV_RSP_BUFFER_CFG, &cfg);
}

/*
 * The resource configuration, checked against the real nrfxlib headers.
 *
 * Everything else that touches hci_sdc_resources.h is compiled against the
 * fake sdc.h under stubs, which is written by hand. That fake holds copies
 * of the SDC_MEM_ macros and of the sdc_cfg_t members this firmware writes,
 * and nothing has ever compared those copies with the vendor header. So a
 * renamed field, a retyped union member, or a memory macro that moved between
 * nrfxlib releases would pass every test here and fail, or worse silently
 * under reserve, on the target.
 *
 * This closes that. It builds only with NRFXLIB_DIR set, and it does three
 * things the stub build cannot:
 *
 *   it computes the pool from the real macros, so a vendor change to any of
 *   them shows up as a number that moved rather than as a firmware that will
 *   not enable;
 *
 *   it writes every sdc_cfg_t member hci_sdc_resources.cpp writes, through the
 *   same SDC_CFG_TYPE_ value, so a rename or a type change is a compile error
 *   here instead of on the target;
 *
 *   it pins each term of the pool to the number the header comment and the
 *   README both quote, so those cannot quietly become wrong.
 *
 * Failing here does not mean the firmware is broken. It means the numbers
 * written down no longer match the library, and the two have to be
 * reconciled before the next release.
 */

#include "hci_sdc_resources.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sdc.h"

/*
 * The per term costs, written out rather than derived, exactly as
 * include/hci_sdc_resources.h and the README quote them. These are what an
 * nrfxlib upgrade is most likely to move. The same file is measured against
 * the fake sdc.h by hci_nrf52840_usb_test, so the fake and the vendor header
 * cannot disagree without one of the two failing.
 */
#include "hci_sdc_expected_resources.h"

static int gFailures;

static void Check(const char *label, long actual, long expected)
{
    if (actual == expected)
    {
        printf("[ok] %-38s %ld\n", label, actual);
        return;
    }

    printf("[!!] %-38s %ld, the source says %ld\n", label, actual, expected);
    gFailures++;
}

int main(void)
{
    const unsigned links =
        HCI_SDC_PERIPHERAL_COUNT + HCI_SDC_CENTRAL_COUNT;

    /*
     * Per term, so a failure names which one moved rather than only saying the
     * total is wrong.
     */
    Check("peripheral link",
          SDC_MEM_PER_PERIPHERAL_LINK(HCI_SDC_ACL_PACKET_SIZE,
                                      HCI_SDC_ACL_PACKET_SIZE,
                                      HCI_SDC_ACL_PACKET_COUNT,
                                      HCI_SDC_ACL_PACKET_COUNT),
          EXPECT_PERIPHERAL_LINK);
    Check("central link",
          SDC_MEM_PER_CENTRAL_LINK(HCI_SDC_ACL_PACKET_SIZE,
                                   HCI_SDC_ACL_PACKET_SIZE,
                                   HCI_SDC_ACL_PACKET_COUNT,
                                   HCI_SDC_ACL_PACKET_COUNT),
          EXPECT_CENTRAL_LINK);
    Check("advertising set",
          SDC_MEM_PER_ADV_SET(HCI_SDC_MAX_ADV_DATA), EXPECT_ADV_SET);
    Check("scan buffers",
          SDC_MEM_SCAN_EXT(HCI_SDC_SCAN_BUFFER_COUNT),
          EXPECT_SCAN_BUFFERS);
    Check("filter accept list", SDC_MEM_FAL(HCI_SDC_FAL_SIZE),
          EXPECT_ACCEPT_LIST);

    Check("channel survey", SDC_MEM_QOS_CHANNEL_SURVEY, EXPECT_CHANNEL_SURVEY);
    Check("power control", SDC_MEM_LE_POWER_CONTROL(links),
          EXPECT_POWER_CONTROL);
    Check("subrating", SDC_MEM_SUBRATING(links), EXPECT_SUBRATING);
    Check("extended features",
          SDC_MEM_EXTENDED_FEATURE_SET(links,
                                       HCI_SDC_EXTENDED_FEATURE_PAGES),
          EXPECT_EXTENDED_FEAT);
    Check("frame space update", SDC_MEM_FRAME_SPACE_UPDATE(links),
          EXPECT_FRAME_SPACE);
    Check("shorter connection intervals",
          SDC_MEM_SHORTER_CONNECTION_INTERVALS(links), EXPECT_SHORTER_CONN);
    Check("scan and initiate together", SDC_MEM_INITIATOR, EXPECT_PARALLEL);
    Check("periodic adv set",
          SDC_MEM_PER_PERIODIC_ADV_SET(HCI_SDC_MAX_ADV_DATA),
          EXPECT_PERIODIC_ADV_SET);
    /*
     * The per sync cost, which the responding scanner raises for every sync
     * rather than adding a term of its own. HCI_SDC_MEM_PER_SYNC is
     * whichever of the two vendor macros applies.
     */
    Check("periodic sync", HCI_SDC_MEM_PER_SYNC,
          EXPECT_PERIODIC_SYNC);
    Check("periodic adv list",
          SDC_MEM_PERIODIC_ADV_LIST(HCI_SDC_PERIODIC_ADV_LIST_SIZE),
          EXPECT_PERIODIC_ADV_LIST);
    Check("periodic sync transfer", SDC_MEM_SYNC_TRANSFER(links),
          EXPECT_SYNC_TRANSFER);
    Check("periodic adv set with responses",
          SDC_MEM_PER_PERIODIC_ADV_RSP_SET(
              HCI_SDC_MAX_ADV_DATA,
              HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS,
              HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS,
              HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA,
              HCI_SDC_PERIODIC_ADV_RSP_FAILURE_REPORTING),
          EXPECT_PERIODIC_ADV_RSP);

    /*
     * Isochronous channels. Encryption does not enter into these figures:
     * the pool pays for the streams and the buffers whether or not the part
     * can encrypt what goes over them.
     */
    Check("iso connected group", SDC_MEM_PER_CIG(HCI_SDC_CIG_COUNT),
          EXPECT_ISO_CIG);
    Check("iso connected streams", SDC_MEM_PER_CIS(HCI_SDC_CIS_COUNT),
          EXPECT_ISO_CIS);
    Check("iso broadcast group", SDC_MEM_PER_BIG(HCI_SDC_BIG_COUNT),
          EXPECT_ISO_BIG);
    Check("iso broadcast streams",
          SDC_MEM_PER_BIS(HCI_SDC_BIS_SOURCE_COUNT +
                          HCI_SDC_BIS_SINK_COUNT),
          EXPECT_ISO_BIS);
    Check("iso rx pdu pool",
          SDC_MEM_ISO_RX_PDU_POOL_PER_STREAM_SIZE(
              HCI_SDC_ISO_RX_PDU_PER_STREAM, HCI_SDC_CIS_COUNT,
              HCI_SDC_BIS_SINK_COUNT),
          EXPECT_ISO_RX_PDU);
    Check("iso rx sdu pool",
          SDC_MEM_ISO_RX_SDU_POOL_SIZE(HCI_SDC_ISO_RX_SDU_COUNT,
                                       HCI_SDC_ISO_RX_SDU_SIZE),
          EXPECT_ISO_RX_SDU);
    Check("iso tx pdu pool",
          SDC_MEM_ISO_TX_PDU_POOL_SIZE(HCI_SDC_ISO_TX_PDU_PER_STREAM,
                                       HCI_SDC_CIS_COUNT,
                                       HCI_SDC_BIS_SOURCE_COUNT),
          EXPECT_ISO_TX_PDU);
    Check("iso tx sdu pool",
          SDC_MEM_ISO_TX_SDU_POOL_SIZE(HCI_SDC_ISO_TX_SDU_COUNT,
                                       HCI_SDC_ISO_TX_SDU_SIZE),
          EXPECT_ISO_TX_SDU);
    Check("iso all terms", HCI_SDC_MEM_ISO, EXPECT_ISO_TOTAL);

    /*
     * What the application has to hand sdc_hci_get. The specification allows
     * an isochronous packet of 4095 octets, but sdc.h ties the requirement to
     * the configured receive size, so this is the number that matters and it
     * is far smaller.
     */
    Check("iso packet from the configured sdu", HCI_SDC_ISO_PACKET_SIZE,
          HCI_SDC_ISO_RX_SDU_SIZE + HCI_ISO_DATA_HEADER_SIZE);

    Check("pool required", HCI_SDC_MEM_REQUIRED, EXPECT_REQUIRED);
    Check("pool allocated", HCI_SDC_MEM_SIZE,
          EXPECT_REQUIRED + HCI_SDC_MEM_MARGIN);

    /*
     * The margin exists because sdc.h says the memory macros may move between
     * minor releases, and the number that decides whether the controller
     * starts is the one sdc_cfg_set answers at run time. A margin smaller than
     * the largest single term would not absorb one of them changing, so it is
     * worth stating that it is a cushion and not a guarantee.
     */
    assert(HCI_SDC_MEM_MARGIN > 0U);

    /*
     * Every configuration this firmware sets, written through the real union.
     * The point is the compiler rather than the values: a member that is
     * renamed or retyped in a future nrfxlib fails to build here, next to the
     * pool that assumed it, instead of on the target.
     */
    {
        sdc_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));

        cfg.buffer_cfg.rx_packet_size = HCI_SDC_ACL_PACKET_SIZE;
        cfg.buffer_cfg.tx_packet_size = HCI_SDC_ACL_PACKET_SIZE;
        cfg.buffer_cfg.rx_packet_count = HCI_SDC_ACL_PACKET_COUNT;
        cfg.buffer_cfg.tx_packet_count = HCI_SDC_ACL_PACKET_COUNT;
        cfg.peripheral_count.count = HCI_SDC_PERIPHERAL_COUNT;
        cfg.central_count.count = HCI_SDC_CENTRAL_COUNT;
        cfg.adv_count.count = HCI_SDC_ADV_SET_COUNT;
        cfg.adv_buffer_cfg.max_adv_data = HCI_SDC_MAX_ADV_DATA;
        cfg.scan_buffer_cfg.count = HCI_SDC_SCAN_BUFFER_COUNT;

        /* A bare integer in the real header, not a role count. */
        cfg.fal_size = HCI_SDC_FAL_SIZE;

        cfg.extended_feature_page_count = HCI_SDC_EXTENDED_FEATURE_PAGES;
        cfg.periodic_adv_count.count = HCI_SDC_PERIODIC_ADV_COUNT;
        cfg.periodic_sync_count.count = HCI_SDC_PERIODIC_SYNC_COUNT;
        cfg.periodic_sync_buffer_cfg.count =
            HCI_SDC_PERIODIC_SYNC_BUFFER_COUNT;
        cfg.periodic_adv_list_size = HCI_SDC_PERIODIC_ADV_LIST_SIZE;
        cfg.periodic_adv_rsp_count.count = HCI_SDC_PERIODIC_ADV_RSP_COUNT;
        cfg.periodic_adv_rsp_buffer_cfg.tx_buffer_count =
            HCI_SDC_PERIODIC_ADV_RSP_TX_BUFFERS;
        cfg.periodic_adv_rsp_buffer_cfg.rx_buffer_count =
            HCI_SDC_PERIODIC_ADV_RSP_RX_BUFFERS;
        cfg.periodic_adv_rsp_buffer_cfg.max_tx_data_size =
            HCI_SDC_PERIODIC_ADV_RSP_MAX_TX_DATA;
        cfg.periodic_adv_rsp_failure_reporting_cfg =
            HCI_SDC_PERIODIC_ADV_RSP_FAILURE_REPORTING;
        cfg.periodic_sync_rsp_tx_buffer_cfg.count =
            HCI_SDC_PERIODIC_SYNC_RSP_TX_BUFFERS;
        cfg.cig_count.count = HCI_SDC_CIG_COUNT;
        cfg.cis_count.count = HCI_SDC_CIS_COUNT;
        cfg.big_count.count = HCI_SDC_BIG_COUNT;
        cfg.bis_source_count.count = HCI_SDC_BIS_SOURCE_COUNT;
        cfg.bis_sink_count.count = HCI_SDC_BIS_SINK_COUNT;
        cfg.iso_buffer_cfg.tx_sdu_buffer_count = HCI_SDC_ISO_TX_SDU_COUNT;
        cfg.iso_buffer_cfg.tx_sdu_buffer_size = HCI_SDC_ISO_TX_SDU_SIZE;
        cfg.iso_buffer_cfg.tx_pdu_buffer_per_stream_count =
            HCI_SDC_ISO_TX_PDU_PER_STREAM;
        cfg.iso_buffer_cfg.rx_pdu_buffer_per_stream_count =
            HCI_SDC_ISO_RX_PDU_PER_STREAM;
        cfg.iso_buffer_cfg.rx_sdu_buffer_count = HCI_SDC_ISO_RX_SDU_COUNT;
        cfg.iso_buffer_cfg.rx_sdu_buffer_size = HCI_SDC_ISO_RX_SDU_SIZE;

        /*
         * The tags, in the same order hci_sdc_resources.cpp uses them. Distinct
         * values, so a header that collapsed two of them would be caught.
         */
        const uint8_t tags[] = {
            SDC_CFG_TYPE_CIG_COUNT,       SDC_CFG_TYPE_CIS_COUNT,
            SDC_CFG_TYPE_BIG_COUNT,       SDC_CFG_TYPE_BIS_SOURCE_COUNT,
            SDC_CFG_TYPE_BIS_SINK_COUNT,  SDC_CFG_TYPE_ISO_BUFFER_CFG,
            SDC_CFG_TYPE_BUFFER_CFG,      SDC_CFG_TYPE_PERIPHERAL_COUNT,
            SDC_CFG_TYPE_CENTRAL_COUNT,   SDC_CFG_TYPE_ADV_COUNT,
            SDC_CFG_TYPE_ADV_BUFFER_CFG,  SDC_CFG_TYPE_SCAN_BUFFER_CFG,
            SDC_CFG_TYPE_FAL_SIZE,
            SDC_CFG_TYPE_EXTENDED_FEATURE_PAGE_COUNT,
            SDC_CFG_TYPE_PERIODIC_ADV_COUNT,
            SDC_CFG_TYPE_PERIODIC_SYNC_COUNT,
            SDC_CFG_TYPE_PERIODIC_SYNC_BUFFER_CFG,
            SDC_CFG_TYPE_PERIODIC_ADV_LIST_SIZE,
            SDC_CFG_TYPE_PERIODIC_ADV_RSP_COUNT,
            SDC_CFG_TYPE_PERIODIC_ADV_RSP_BUFFER_CFG,
            SDC_CFG_TYPE_PERIODIC_ADV_RSP_FAILURE_REPORTING_CFG,
            SDC_CFG_TYPE_PERIODIC_SYNC_RSP_TX_BUFFER_CFG,
        };
        const size_t tagCount = sizeof(tags) / sizeof(tags[0]);

        for (size_t i = 0U; i < tagCount; i++)
        {
            assert(tags[i] != SDC_CFG_TYPE_NONE);
            for (size_t j = i + 1U; j < tagCount; j++)
            {
                if (tags[i] == tags[j])
                {
                    printf("[!!] configuration tags %u and %u are the same\n",
                           (unsigned)i, (unsigned)j);
                    gFailures++;
                }
            }
        }

        printf("[ok] %-38s %u distinct\n", "configuration tags",
               (unsigned)tagCount);
    }

    /*
     * A page count the controller cannot hold would reserve memory for pages
     * it will never fill. Ten is the sdk-nrfxlib default and the largest this
     * has been checked against.
     */
    assert(HCI_SDC_EXTENDED_FEATURE_PAGES > 0U);
    assert(HCI_SDC_EXTENDED_FEATURE_PAGES <=
           SDC_DEFAULT_EXTENDED_FEATURE_PAGE_COUNT);

    /*
     * A periodic advertiser needs an advertising set to carry it. The header
     * refuses this at build time; asserting it here as well means the rule is
     * stated where the numbers are, for anyone reading only this file.
     */
    assert((HCI_SDC_PERIODIC_ADV_COUNT +
            HCI_SDC_PERIODIC_ADV_RSP_COUNT) <=
           HCI_SDC_ADV_SET_COUNT);

    printf("\n");
    if (gFailures != 0)
    {
        printf("%d resource number(s) disagree with this nrfxlib. The pool is "
               "computed from the library, so the firmware is not broken, but "
               "include/hci_sdc_resources.h and the README quote numbers that "
               "are no longer true.\n",
               gFailures);
        return 1;
    }

    printf("All SDC resource tests passed.\n");
    return 0;
}

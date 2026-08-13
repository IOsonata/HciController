/*
 * What the resource configuration is supposed to cost.
 *
 * Shared by two tests that reach these numbers through different headers:
 *
 *   hci_sdc_resources_test   builds against the real nrfxlib sdc.h
 *   hci_nrf52840_usb_test    builds against the fake one under stubs/sdc
 *
 * That is the point. The fake holds hand written copies of the SDC_MEM_
 * macros, and nothing used to compare them with the vendor ones. With both
 * tests measured against this file, a fake that drifts fails, a vendor header
 * that moves fails, and the two failing separately says which one moved.
 *
 * These are also the numbers include/hci_sdc_resources.h and the README
 * quote, so a change here is a change those two need as well.
 */

#ifndef HCI_SDC_EXPECTED_RESOURCES_H
#define HCI_SDC_EXPECTED_RESOURCES_H

#define EXPECT_PERIPHERAL_LINK 2951
#define EXPECT_CENTRAL_LINK    2855
#define EXPECT_ADV_SET          961
#define EXPECT_SCAN_BUFFERS    1688
#define EXPECT_ACCEPT_LIST       68
#define EXPECT_CHANNEL_SURVEY    40
#define EXPECT_POWER_CONTROL   2227
#define EXPECT_SUBRATING       1092
#define EXPECT_EXTENDED_FEAT   4673
#define EXPECT_FRAME_SPACE     1236
#define EXPECT_SHORTER_CONN     948
#define EXPECT_PARALLEL         384

/*
 * Periodic advertising, per unit rather than per total, so a failure names
 * which one moved. The totals follow from the counts in
 * hci_sdc_resources.h.
 */
#define EXPECT_PERIODIC_ADV_SET   753   /* each, at 255 octets of data */
#define EXPECT_PERIODIC_SYNC     1787   /* each, four rx buffers, with responses */
#define EXPECT_PERIODIC_ADV_LIST   64   /* eight entries */
#define EXPECT_SYNC_TRANSFER     2515   /* eighteen links */
#define EXPECT_PERIODIC_ADV_RSP  1575   /* each, one tx and one rx buffer */

/* Isochronous channels, per term so a failure names which one moved. */
#define EXPECT_ISO_CIG           339
#define EXPECT_ISO_CIS          2201
#define EXPECT_ISO_BIG           675
#define EXPECT_ISO_BIS          1049
#define EXPECT_ISO_RX_PDU       5280
#define EXPECT_ISO_RX_SDU       1064
#define EXPECT_ISO_TX_PDU       5280
#define EXPECT_ISO_TX_SDU       1196
#define EXPECT_ISO_TOTAL       17084

/* Core 6.0 is 91584. FSU + SCI add 1236 + 948 for the Core 6.2 profile. */
#define EXPECT_REQUIRED_6_0   91584
#define EXPECT_REQUIRED_6_2   93768
#define EXPECT_REQUIRED       EXPECT_REQUIRED_6_2

#endif

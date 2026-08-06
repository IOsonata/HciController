/*
 * What the resource configuration is supposed to cost.
 *
 * Shared by two tests that reach these numbers through different headers:
 *
 *   hci_nrf52840_resources_test  builds against the real nrfxlib sdc.h
 *   hci_nrf52840_usb_test        builds against the fake one under stubs/sdc
 *
 * That is the point. The fake carries hand written copies of the SDC_MEM_
 * macros, and nothing used to compare them with the vendor ones. With both
 * tests measured against this file, a fake that drifts fails, a vendor header
 * that moves fails, and the two failing separately says which one moved.
 *
 * These are also the numbers include/hci_nrf52840.h and the README quote, so
 * a change here is a change those two need as well.
 */

#ifndef HCI_NRF52840_EXPECTED_RESOURCES_H
#define HCI_NRF52840_EXPECTED_RESOURCES_H

#define EXPECT_PERIPHERAL_LINK 2935
#define EXPECT_CENTRAL_LINK    2839
#define EXPECT_ADV_SET          961
#define EXPECT_SCAN_BUFFERS    1688
#define EXPECT_ACCEPT_LIST       68
#define EXPECT_CHANNEL_SURVEY    40
#define EXPECT_POWER_CONTROL    997
#define EXPECT_SUBRATING        492
#define EXPECT_EXTENDED_FEAT   2083
#define EXPECT_PARALLEL         384

#define EXPECT_REQUIRED       30808

#endif

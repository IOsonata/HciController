/**-------------------------------------------------------------------------
@file	hci_version.h

@brief	HciController firmware and USB release version constants.

@author	Nguyen Hoan Hoang
@date	August 2026

@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/

#ifndef HCI_VERSION_H
#define HCI_VERSION_H

/* IOsonata Vers_t format: 0xMMmm, where MM is major and mm is minor. */
#define FIRMWARE_VERSION 0x0100U

/* USB bcdDevice is 1.00 for HciController 1.0.0. */
#define HCI_CONTROLLER_VERSION_BCD FIRMWARE_VERSION

#endif

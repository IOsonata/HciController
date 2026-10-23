/**-------------------------------------------------------------------------
@file	hci_vendor_nrf.cpp
@brief	Nordic side of the I-SYST vendor commands.

The static address is the factory device address, two top bits set as the
Core specification asks of a static random address, and the entropy source
is the one the link layer draws on.

@author	Nguyen Hoan Hoang
@date	October 2026
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#include "hci_vendor.h"

#include "nrf.h"
#include "crypto_rng_nrf.h"

#if defined(NRF5340_XXAA_NETWORK) && !defined(NRF_FICR)
#define NRF_FICR    NRF_FICR_NS
#endif

extern "C" bool HciVendorStaticAddr(uint8_t Addr[6])
{
    const uint32_t lo = NRF_FICR->DEVICEADDR[0];
    const uint32_t hi = NRF_FICR->DEVICEADDR[1];

    Addr[0] = (uint8_t)lo;
    Addr[1] = (uint8_t)(lo >> 8);
    Addr[2] = (uint8_t)(lo >> 16);
    Addr[3] = (uint8_t)(lo >> 24);
    Addr[4] = (uint8_t)hi;
    Addr[5] = (uint8_t)((hi >> 8) | 0xC0U);

    return true;
}

RngEngine *HciVendorRng(void)
{
    return CryptoRngNrfInstance();
}

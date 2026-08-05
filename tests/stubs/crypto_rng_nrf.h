#ifndef STUB_CRYPTO_RNG_NRF_H
#define STUB_CRYPTO_RNG_NRF_H
#include <stdint.h>
#include <stddef.h>
typedef enum { CRYPTO_STATUS_OK = 0, CRYPTO_STATUS_ERROR = 1 } CRYPTO_STATUS;
class CryptoRngNrf {
public:
    CRYPTO_STATUS Random(uint8_t *pBuff, size_t Len);
};
CryptoRngNrf *CryptoRngNrfInstance(void);
#endif

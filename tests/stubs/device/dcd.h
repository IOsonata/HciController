#ifndef STUB_DCD_H
#define STUB_DCD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void dcd_connect(uint8_t rhport);
void dcd_disconnect(uint8_t rhport);
#ifdef __cplusplus
}
#endif
#endif

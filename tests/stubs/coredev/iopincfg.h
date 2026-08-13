/*
 * Enough of the IOsonata pin configuration for a host build.
 *
 * Only the shape the application uses: it fills an array of these from the
 * board and hands it to UARTInit. Nothing here configures anything.
 */

#ifndef IOPINCFG_H__
#define IOPINCFG_H__

#include <stdint.h>

typedef enum {
    IOPINDIR_INPUT = 0,
    IOPINDIR_OUTPUT = 1,
    IOPINDIR_BI = 2,
} IOPINDIR;

typedef enum {
    IOPINRES_NONE = 0,
    IOPINRES_PULLUP = 1,
    IOPINRES_PULLDOWN = 2,
    IOPINRES_FOLLOW = 3,
} IOPINRES;

typedef enum {
    IOPINTYPE_NORMAL = 0,
    IOPINTYPE_OPENDRAIN = 1,
} IOPINTYPE;

typedef struct {
    int PortNo;
    int PinNo;
    int PinOp;
    IOPINDIR PinDir;
    IOPINRES Res;
    IOPINTYPE Type;
} IOPinCfg_t;

#ifdef __cplusplus
extern "C" {
#endif

void IOPinCfg(const IOPinCfg_t *pCfg, int NbPins);
void IOPinDisable(int PortNo, int PinNo);

/*
 * The single pin form, which the flow control probe uses to hold a pin with a
 * pull up and then a pull down. Same argument order as the real header.
 */
void IOPinConfig(int PortNo, int PinNo, int PinOp, IOPINDIR Dir,
                 IOPINRES Resistor, IOPINTYPE Type);

#ifdef __cplusplus
}
#endif

#endif /* IOPINCFG_H__ */

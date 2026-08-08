/*
 * Enough of the IOsonata pin control for a host build.
 *
 * The real header is ARM/Nordic/include/iopinctrl.h, where IOPinRead is a
 * always inline function over the GPIO IN register. Here it is a plain
 * declaration so a test can supply pin levels of its own.
 */

#ifndef IOPINCTRL_H__
#define IOPINCTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

int IOPinRead(int PortNo, int PinNo);

#ifdef __cplusplus
}
#endif

#endif /* IOPINCTRL_H__ */

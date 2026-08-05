/* Minimal stand-in so the SDC headers can be compiled for an API check. */
#ifndef STUB_NRFX_H
#define STUB_NRFX_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifndef __PACKED
#define __PACKED __attribute__((packed))
#endif
#ifndef __ALIGN
#define __ALIGN(n) __attribute__((aligned(n)))
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#endif

/*
 * ReliefOS kernel type definitions: supplies freestanding integer and ABI types.
 * Centralizes common aliases used by all kernel headers and implementations.
 */
#ifndef RELIEFNT_TYPES_H
#define RELIEFNT_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * x86 FPU save areas are kept at a fixed maximum size in task objects.  The
 * legacy FXSAVE image is 512 bytes; the user x87/SSE/AVX XSAVE subset is 832
 * bytes.  Keep one extra 64-byte slot so a future architecture implementation
 * can grow the selected state without changing the task allocation contract.
 */
#define RELIEFNT_FPU_LEGACY_STATE_SIZE 512u
#define RELIEFNT_FPU_STATE_MAX_SIZE 1024u

#endif

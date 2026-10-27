/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RELIEFNT_MODPOST_OFFSET_TYPES_H
#define RELIEFNT_MODPOST_OFFSET_TYPES_H
/* Target ABI types for the unchanged upstream devicetable-offsets probe.
 * This header is a build-tool input and is never linked into kernel/modules. */
#include <stddef.h>
#include <stdint.h>
#include <linux/types.h>
#define BITS_PER_LONG 64
#define UUID_STRING_LEN 36
_Static_assert(sizeof(unsigned long)==8,"ReliefNT module tools target x86-64");
typedef unsigned long kernel_ulong_t;
typedef struct { __u8 b[16]; } uuid_le;
typedef struct { __u8 b[16]; } uuid_t;
typedef struct { __u8 b[16]; } guid_t;
#endif

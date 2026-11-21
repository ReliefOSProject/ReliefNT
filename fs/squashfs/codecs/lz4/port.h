/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_LZ4_PORT_H
#define RELIEFNT_LZ4_PORT_H
#define LZ4_FREESTANDING 1
#define LZ4_memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define LZ4_memset(d, v, n) __builtin_memset((d), (v), (n))
#define LZ4_memmove(d, s, n) __builtin_memmove((d), (s), (n))
#endif

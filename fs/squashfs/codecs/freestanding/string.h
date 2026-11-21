/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SQUASHFS_FREESTANDING_STRING_H
#define RELIEFNT_SQUASHFS_FREESTANDING_STRING_H

#include <stddef.h>

/* Provided by the kernel's lib/string.c. */
void *memcpy(void *dst, const void *src, size_t len);
void *memmove(void *dst, const void *src, size_t len);
void *memset(void *dst, int value, size_t len);
int memcmp(const void *a, const void *b, size_t len);

#endif

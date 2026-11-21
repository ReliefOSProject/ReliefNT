/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_ZSTD_PORT_H
#define RELIEFNT_ZSTD_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void *kernel_malloc(size_t size);
void kernel_free(void *ptr);

static inline void *sqfs_zstd_calloc(size_t count, size_t width)
{
    if (width && count > SIZE_MAX / width) return NULL;
    size_t bytes = count * width;
    void *buffer = kernel_malloc(bytes);
    if (buffer) memset(buffer, 0, bytes);
    return buffer;
}

#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(size) kernel_malloc(size)
#define ZSTD_calloc(count, width) sqfs_zstd_calloc((count), (width))
#define ZSTD_free(ptr) kernel_free(ptr)
#define ZSTD_NO_INTRINSICS 1
#define XXH_NO_STDLIB
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_STREAM_H
#define RELIEFNT_SVGA_STREAM_H

#include "protocol/svga_reg.h"
#include <linux/errno.h>
#include <stdbool.h>
#include <stdint.h>

struct svga_packet {
    uint32_t command, bytes;
    void *body, *next;
};

/* Command storage and each packet header are dword aligned. */
static inline int svga_packet_read(void *data, uint32_t remaining, struct svga_packet *out)
{
    const uint32_t *header = data;
    if (remaining < 8)
        return -EINVAL;
    uint32_t bytes = header[1];
    if (bytes > SVGA_CMD_MAX_DATASIZE)
        return -E2BIG;
    if ((bytes & 3) || bytes > remaining - 8)
        return -EINVAL;
    *out = (struct svga_packet){
        .command = header[0], .bytes = bytes, .body = (uint8_t *)data + 8,
        .next = (uint8_t *)data + 8 + bytes,
    };
    return 0;
}

static inline int svga_array_count(uint32_t bytes, uint32_t prefix, uint32_t stride,
                                   uint32_t limit)
{
    if (bytes < prefix || !stride)
        return -1;
    uint32_t tail = bytes - prefix;
    return tail % stride || tail / stride > limit ? -1 : (int)(tail / stride);
}

static inline bool svga_buffer_fits(uint64_t size, uint64_t offset, uint64_t bytes)
{
    return offset <= size && bytes <= size - offset;
}

#endif

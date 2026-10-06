/*
 * VMware legacy FIFO ring arithmetic shared by the framebuffer backend and
 * its host-side boundary tests.
 */
#ifndef RELIEFOS_FRAMEBUFFER_FIFO_H
#define RELIEFOS_FRAMEBUFFER_FIFO_H

#include <reliefnt/types.h>

/**
 * @brief Reserve one contiguous command in a legacy FIFO ring.
 * @param fifo_min First byte in the ring, inclusive.
 * @param fifo_max One-past-last byte in the ring.
 * @param next Current producer offset.
 * @param stop Current consumer offset.
 * @param bytes Command length in bytes.
 * @param write_at Output offset at which the command must be written.
 * @param next_after Output producer offset after publication.
 * @return True when the command fits without colliding with STOP.
 *
 * NEXT_CMD == STOP denotes an empty ring, so one byte of command-space must
 * remain unused.  A command is never split at the end of the ring: if its
 * tail does not fit, the reservation starts at FIFO_MIN and leaves the tail
 * as padding.  Exact-tail commands are valid and publish FIFO_MIN.
 */
static inline bool framebuffer_fifo_reserve(uint32_t fifo_min, uint32_t fifo_max,
                                            uint32_t next, uint32_t stop,
                                            uint32_t bytes, uint32_t *write_at,
                                            uint32_t *next_after)
{
    uint64_t capacity;
    uint64_t free_bytes;

    if (!write_at || !next_after || fifo_min >= fifo_max ||
        next < fifo_min || next >= fifo_max || stop < fifo_min ||
        stop >= fifo_max || !bytes) {
        return false;
    }
    capacity = (uint64_t)fifo_max - fifo_min;
    if ((uint64_t)bytes >= capacity) return false;
    free_bytes = next >= stop
        ? (uint64_t)fifo_max - next + stop - fifo_min
        : (uint64_t)stop - next;
    if ((uint64_t)bytes >= free_bytes) return false;

    if (next >= stop && (uint64_t)next + bytes > fifo_max) {
        if ((uint64_t)bytes >= (uint64_t)stop - fifo_min) return false;
        *write_at = fifo_min;
        *next_after = fifo_min + bytes;
        return true;
    }

    *write_at = next;
    *next_after = (uint64_t)next + bytes == fifo_max
                      ? fifo_min : next + bytes;
    return true;
}

#endif

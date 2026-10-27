/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DMA_H
#define RELIEFNT_DMA_H

#include <reliefnt/types.h>

enum dma_direction { DMA_FROM_DEVICE, DMA_TO_DEVICE };

/* Coherent, contiguous physical backing on x86 without an IOMMU. The controller
 * owns this noncopyable object, serializes access and pins its own module while
 * callers can submit I/O. CPU pointers are never device addresses. */
struct dma_buffer {
    void *cpu;
    uint64_t physical;
    size_t capacity, length;
    uint32_t pages;
    enum dma_direction direction;
    bool busy;
};

/**
 * @brief Allocate coherent pages whose entire rounded extent fits a DMA mask.
 * @param buffer Required zeroed, exclusively owned object; remains empty on error.
 * @param bytes Nonzero requested capacity in bytes, rounded up to 4 KiB pages.
 * @param mask Inclusive maximum bus byte address supported by this device.
 * @return Zero, -EINVAL for size, -EBUSY if allocated, -ENOMEM for pages,
 *         -ERANGE for invalid physical extent, or -EFAULT for absent direct map.
 * IRQ-safe allocator, no sleeping or callbacks; caller serializes and later frees.
 */
int dma_buffer_alloc(struct dma_buffer *buffer, size_t bytes, uint64_t mask);

/**
 * @brief Stage one transfer without retaining a borrowed CPU buffer pointer.
 * @param buffer Allocated exclusively locked controller-owned backing.
 * @param direction DMA_FROM_DEVICE for read or DMA_TO_DEVICE for write.
 * @param source Valid kernel bytes for write, required NULL for device read.
 * @param bytes Nonzero transfer length within allocated capacity.
 * @return Zero, -EINVAL for invalid arguments or -EBUSY for a previous transfer.
 * Copies write bytes and orders stores before submission; caller programs hardware.
 */
int dma_buffer_begin(struct dma_buffer *buffer, enum dma_direction direction,
                     const void *source, size_t bytes);

/**
 * @brief Consume/discard a completed transfer only after hardware stopped DMA.
 * @param buffer Busy controller-owned backing under the same exclusive lock.
 * @param destination Current valid kernel read destination, or NULL to discard;
 *                    must be NULL for a write. Never use a stale saved task pointer.
 * @param quiesced True only after completion or successful controller/port STOP.
 * @return Zero, -EINVAL for invalid state/arguments, -EBUSY when not quiesced.
 * Failure retains ownership/busy state; successful read copies after DMA barrier.
 */
int dma_buffer_end(struct dma_buffer *buffer, void *destination, bool quiesced);

/**
 * @brief Release idle backing after the device can no longer access its pages.
 * @param buffer Required exclusively owned object; successful free clears it.
 * @return Zero including an empty object, -EINVAL for NULL, -EBUSY for live DMA.
 * Does not STOP hardware; descriptor/ring owners must STOP before calling it.
 */
int dma_buffer_free(struct dma_buffer *buffer);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/dma.h>
#include <reliefnt/mm.h>
#include <reliefnt/export.h>
#include <linux/errno.h>

#define DMA_PAGE_SIZE 4096u

/** @brief Copy only kernel-owned bytes while the controller lock excludes DMA.
 * @param out Writable nonoverlapping extent of at least bytes.
 * @param in Readable nonoverlapping extent of at least bytes.
 * @param bytes Byte count validated by the caller.
 */
static void dma_copy(void *out, const void *in, size_t bytes)
{
    unsigned char *destination = out;
    const unsigned char *source = in;
    for (size_t i = 0; i < bytes; ++i) destination[i] = source[i];
}

/** @brief Claim contiguous backing; no publication, no hardware or module calls.
 * @param buffer Required zeroed exclusively owned object, unchanged on failure.
 * @param bytes Nonzero requested bytes, rounded up without integer overflow.
 * @param mask Inclusive device bus limit for the complete allocated page run.
 * @return Zero or -EINVAL/-EBUSY/-ENOMEM/-ERANGE/-EFAULT; errors reclaim pages.
 */
int dma_buffer_alloc(struct dma_buffer *buffer, size_t bytes, uint64_t mask)
{
    if (!buffer || !bytes || bytes > SIZE_MAX - (DMA_PAGE_SIZE - 1u))
        return -EINVAL;
    if (buffer->cpu || buffer->physical || buffer->pages || buffer->busy)
        return -EBUSY;
    size_t pages = (bytes + DMA_PAGE_SIZE - 1u) / DMA_PAGE_SIZE;
    if (pages > UINT32_MAX) return -EINVAL;
    size_t capacity = pages * DMA_PAGE_SIZE;
    uint64_t physical = mm_alloc_pages_below((uint32_t)pages, mask);
    if (!physical) return -ENOMEM;
    int error = -ERANGE;
    if ((physical & (DMA_PAGE_SIZE - 1u)) || physical > mask ||
        capacity - 1u > mask - physical) goto fail;
    void *cpu = mm_phys_to_virt(physical);
    if (!cpu || (uintptr_t)cpu > UINTPTR_MAX - (capacity - 1u) ||
        (uintptr_t)mm_phys_to_virt(physical + capacity - 1u) !=
        (uintptr_t)cpu + capacity - 1u) {
        error = -EFAULT;
        goto fail;
    }
    *buffer = (struct dma_buffer){ .cpu = cpu, .physical = physical,
        .capacity = capacity, .pages = (uint32_t)pages };
    return 0;
fail:
    mm_free_pages(physical, (uint32_t)pages);
    return error;
}

/** @brief Stage DMA bytes without saving the caller's temporary CPU pointer.
 * @param buffer Allocated backing serialized by its controller lock.
 * @param direction Read/write direction, one of the two supported enum values.
 * @param source Readable write bytes or NULL for read.
 * @param bytes Nonzero extent within buffer capacity.
 * @return Zero, -EBUSY for live DMA, or -EINVAL; no allocation or sleeping.
 */
int dma_buffer_begin(struct dma_buffer *buffer, enum dma_direction direction,
                     const void *source, size_t bytes)
{
    if (!buffer || !buffer->cpu || !bytes || bytes > buffer->capacity)
        return -EINVAL;
    if (buffer->busy) return -EBUSY;
    if ((direction != DMA_FROM_DEVICE && direction != DMA_TO_DEVICE) ||
        (direction == DMA_FROM_DEVICE && source) ||
        (direction == DMA_TO_DEVICE && !source)) return -EINVAL;
    if (source) dma_copy(buffer->cpu, source, bytes);
    buffer->direction = direction;
    buffer->length = bytes;
    buffer->busy = true;
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

/** @brief Copy a current read result or discard it after proven hardware idle.
 * @param buffer Exclusively locked busy backing, retained on every error.
 * @param destination Writable read bytes of buffer->length or NULL to discard.
 * @param quiesced Hardware has completed or successfully stopped its DMA engine.
 * @return Zero, -EBUSY for still-live DMA, or -EINVAL; no callback/lock/sleep.
 */
int dma_buffer_end(struct dma_buffer *buffer, void *destination, bool quiesced)
{
    if (!buffer || !buffer->cpu || !buffer->busy ||
        (buffer->direction == DMA_TO_DEVICE && destination)) return -EINVAL;
    if (!quiesced) return -EBUSY;
    __asm__ volatile("mfence" ::: "memory");
    if (destination) dma_copy(destination, buffer->cpu, buffer->length);
    buffer->busy = false;
    buffer->length = 0;
    return 0;
}

/** @brief Reclaim only idle, controller-owned physical pages.
 * @param buffer Exclusively owned object; successful release zeroes every field.
 * @return Zero, -EINVAL for NULL, or -EBUSY; failure leaves all backing owned.
 */
int dma_buffer_free(struct dma_buffer *buffer)
{
    if (!buffer) return -EINVAL;
    if (buffer->busy) return -EBUSY;
    if (buffer->pages) mm_free_pages(buffer->physical, buffer->pages);
    *buffer = (struct dma_buffer){0};
    return 0;
}

EXPORT_SYMBOL(dma_buffer_alloc);
EXPORT_SYMBOL(dma_buffer_begin);
EXPORT_SYMBOL(dma_buffer_end);
EXPORT_SYMBOL(dma_buffer_free);

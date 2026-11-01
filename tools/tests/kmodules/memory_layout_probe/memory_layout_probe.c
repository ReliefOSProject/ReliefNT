/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/console.h>
#include <reliefnt/mm.h>
#include <reliefnt/module.h>
#include <linux/errno.h>

#define PROBE_PAGE_BYTES 4096ULL
#define PROBE_HIGH_PAGES 513u
#define PROBE_DMA_PAGES 2u
#define PROBE_HIGH_FLOOR (16ULL * 1024 * 1024 * 1024)

/** @brief Generate a deterministic byte depending on its page and offset.
 * @param page Zero-based index in an owned allocation.
 * @param offset Byte offset within the page.
 * @return Pattern byte for verifying physical storage.
 */
static uint8_t probe_pattern(uint32_t page, uint32_t offset)
{
    return (uint8_t)(0xa5u + page * 131u + offset * 17u);
}

/** @brief Check zero-fill, write and read every byte in owned physical pages.
 * @param physical First physical page in the caller-owned contiguous run.
 * @param pages Nonzero page count entirely owned by the caller.
 * @return 0 after every byte matches, or negative errno on mapping/data failure.
 * Only the supplied pages are touched; volatile accesses force real memory I/O.
 */
static int probe_pages(uint64_t physical, uint32_t pages)
{
    for (uint32_t page = 0; page < pages; ++page) {
        volatile uint8_t *mapping = mm_phys_to_virt(physical + (uint64_t)page * PROBE_PAGE_BYTES);
        if (!mapping) return -EFAULT;
        for (uint32_t offset = 0; offset < PROBE_PAGE_BYTES; ++offset)
            if (mapping[offset] != 0) return -EIO;
        for (uint32_t offset = 0; offset < PROBE_PAGE_BYTES; ++offset)
            mapping[offset] = probe_pattern(page, offset);
    }
    for (uint32_t page = 0; page < pages; ++page) {
        volatile uint8_t *mapping = mm_phys_to_virt(physical + (uint64_t)page * PROBE_PAGE_BYTES);
        if (!mapping) return -EFAULT;
        for (uint32_t offset = 0; offset < PROBE_PAGE_BYTES; ++offset)
            if (mapping[offset] != probe_pattern(page, offset)) return -EIO;
    }
    return 0;
}

/** @brief Exercise bounded high-RAM and DMA32 ownership through exported MM APIs.
 * @return 0 after both allocations validate, otherwise negative errno; all
 * pages are freed on every path before module initialization returns.
 */
static int memory_layout_probe_init(void)
{
    uint64_t high = mm_alloc_pages(PROBE_HIGH_PAGES), dma = 0;
    int result = -ENOMEM;
    const uint64_t high_bytes = PROBE_HIGH_PAGES * PROBE_PAGE_BYTES;
    const uint64_t dma_bytes = PROBE_DMA_PAGES * PROBE_PAGE_BYTES;
    if (!high) goto out;
    if (high <= PROBE_HIGH_FLOOR || high > UINT64_MAX - high_bytes) {
        result = -ERANGE;
        goto out;
    }
    result = probe_pages(high, PROBE_HIGH_PAGES);
    if (result) goto out;
    dma = mm_alloc_pages_below(PROBE_DMA_PAGES, UINT32_MAX);
    if (!dma) { result = -ENOMEM; goto out; }
    if (dma > UINT32_MAX || dma_bytes - 1 > UINT32_MAX - dma) {
        result = -ERANGE;
        goto out;
    }
    result = probe_pages(dma, PROBE_DMA_PAGES);
    if (!result)
        console_info("[memory-probe] high=0x%llx..0x%llx pages=%u dma32=0x%llx..0x%llx pages=%u zero=pass pattern=pass\n",
                     (unsigned long long)high, (unsigned long long)(high + high_bytes - 1),
                     PROBE_HIGH_PAGES, (unsigned long long)dma,
                     (unsigned long long)(dma + dma_bytes - 1), PROBE_DMA_PAGES);
out:
    if (result)
        console_error("[memory-probe] failed rc=%d high=0x%llx dma32=0x%llx\n", result,
                      (unsigned long long)high, (unsigned long long)dma);
    if (dma) mm_free_pages(dma, PROBE_DMA_PAGES);
    if (high) mm_free_pages(high, PROBE_HIGH_PAGES);
    return result;
}

/** @brief Unload a probe that released all page ownership during initialization. */
static void memory_layout_probe_exit(void) { }

module_init(memory_layout_probe_init);
module_exit(memory_layout_probe_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("Bounded high RAM and DMA32 physical-page probe");

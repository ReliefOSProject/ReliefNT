/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kmodules/memory_layout_probe/memory_layout_probe.c"

static uint8_t high_pages[PROBE_HIGH_PAGES * PROBE_PAGE_BYTES];
static uint8_t dma_pages[PROBE_DMA_PAGES * PROBE_PAGE_BYTES];
static uint64_t high_return, dma_return;
static unsigned high_frees, dma_frees, map_calls[PROBE_HIGH_PAGES];
static int fail_mapping, corrupt_readback;

/** @brief Return the scenario's preselected contiguous high allocation.
 * @param pages Required page count.
 * @return Fixture physical address or zero for allocation failure.
 */
uint64_t mm_alloc_pages(uint32_t pages) { assert(pages == PROBE_HIGH_PAGES); return high_return; }
/** @brief Return the scenario's preselected DMA allocation.
 * @param pages Required page count. @param mask Required inclusive device mask.
 * @return Fixture physical address or zero for allocation failure.
 */
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{ assert(pages == PROBE_DMA_PAGES && mask == UINT32_MAX); return dma_return; }
/** @brief Track release of every fixture allocation.
 * @param physical Exact owned physical start. @param pages Original page count.
 */
void mm_free_pages(uint64_t physical, uint32_t pages)
{
    if (physical == high_return) { assert(pages == PROBE_HIGH_PAGES); ++high_frees; }
    else { assert(physical == dma_return && pages == PROBE_DMA_PAGES); ++dma_frees; }
}
/** @brief Resolve only fixture-owned pages, optionally injecting read failures.
 * @param physical Page-aligned physical byte supplied by the probe.
 * @return Borrowed volatile-capable backing pointer, or NULL for injected failure.
 */
void *mm_phys_to_virt(uint64_t physical)
{
    if (physical >= high_return && physical - high_return < sizeof(high_pages)) {
        unsigned page = (unsigned)((physical - high_return) / PROBE_PAGE_BYTES);
        if ((int)page == fail_mapping) return NULL;
        if (++map_calls[page] == 2 && (int)page == corrupt_readback)
            high_pages[page * PROBE_PAGE_BYTES + 7] ^= 1;
        return high_pages + (physical - high_return);
    }
    if (physical >= dma_return && physical - dma_return < sizeof(dma_pages))
        return dma_pages + (physical - dma_return);
    return NULL;
}
/** @brief Discard diagnostic messages while testing probe behavior.
 * @param level Log severity. @param format Borrowed format and arguments.
 */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

/** @brief Reset independent allocation and corruption controls for a scenario. */
static void reset_fixture(void)
{
    memset(high_pages, 0, sizeof(high_pages));
    memset(dma_pages, 0, sizeof(dma_pages));
    memset(map_calls, 0, sizeof(map_calls));
    high_return = 0x500000000ULL;
    dma_return = 0xffffe000ULL;
    high_frees = dma_frees = 0;
    fail_mapping = corrupt_readback = -1;
}

/** @brief Exercise success, ownership cleanup and failures at each probe stage.
 * @return Zero after all assertions pass.
 */
int main(void)
{
    reset_fixture();
    assert(!memory_layout_probe_init() && high_frees == 1 && dma_frees == 1);
    for (unsigned page = 0; page < PROBE_HIGH_PAGES; ++page) assert(map_calls[page] == 2);
    assert(dma_pages[sizeof(dma_pages) - 1] == probe_pattern(1, PROBE_PAGE_BYTES - 1));

    reset_fixture(); high_return = 0;
    assert(memory_layout_probe_init() < 0 && !high_frees && !dma_frees);
    reset_fixture(); high_return = PROBE_HIGH_FLOOR;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && !dma_frees);
    reset_fixture(); dma_return = 0;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && !dma_frees);
    reset_fixture(); dma_return = 0xfffff000ULL;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && dma_frees == 1);
    reset_fixture(); high_pages[4 * PROBE_PAGE_BYTES + 77] = 1;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && !dma_frees);
    reset_fixture(); fail_mapping = 400;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && !dma_frees);
    reset_fixture(); corrupt_readback = 512;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && !dma_frees);
    reset_fixture(); dma_pages[sizeof(dma_pages) - 1] = 1;
    assert(memory_layout_probe_init() < 0 && high_frees == 1 && dma_frees == 1);
    puts("PASS bounded probe touches all owned bytes and releases every failure path");
    return 0;
}

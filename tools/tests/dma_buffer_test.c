/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/dma.h>
#include <reliefnt/mm.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The only emulated boundary is the physical page allocator/direct map. */
static unsigned char *allocation;
static uint64_t physical = 0x120000;
static uint64_t observed_mask;
static uint32_t pages;
static unsigned allocations, releases;
static int allocation_failure, mapping_failure;

uint64_t mm_alloc_pages_below(uint32_t count, uint64_t mask)
{
    observed_mask = mask;
    assert(!allocation && count && count < 256);
    if (allocation_failure) return 0;
    pages = count;
    allocation = calloc(count, 4096);
    assert(allocation);
    ++allocations;
    return physical;
}

void *mm_phys_to_virt(uint64_t address)
{
    if (mapping_failure || !allocation || address < physical ||
        address - physical >= (uint64_t)pages * 4096) return NULL;
    return allocation + (address - physical);
}

void mm_free_pages(uint64_t address, uint32_t count)
{
    assert(address == physical && count == pages && allocation);
    free(allocation);
    allocation = NULL;
    ++releases;
}

int main(void)
{
    struct dma_buffer dma = {0};
    unsigned char source[8194], output[8194];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 37u);
    memset(output, 0x9a, sizeof(output));
    assert(dma_buffer_alloc(NULL, 1, UINT32_MAX) == -22);
    assert(dma_buffer_alloc(&dma, 0, UINT32_MAX) == -22);
    assert(dma_buffer_alloc(&dma, SIZE_MAX, UINT64_MAX) == -22);
    assert(!allocations);
    allocation_failure = 1;
    assert(dma_buffer_alloc(&dma, 4096, UINT32_MAX) == -12);
    assert(!dma.cpu && !dma.physical && !dma.pages);
    allocation_failure = 0;
    mapping_failure = 1;
    assert(dma_buffer_alloc(&dma, 8192, UINT32_MAX) == -14);
    assert(allocations == releases && !dma.cpu);
    mapping_failure = 0;
    physical = 0xfffff000ull;
    assert(dma_buffer_alloc(&dma, 4097, UINT32_MAX) == -34);
    assert(allocations == releases && !dma.cpu);
    physical = 0x120000;
    assert(dma_buffer_alloc(&dma, 8192, UINT32_MAX) == 0);
    assert(observed_mask == UINT32_MAX && dma.pages == 2);
    assert(dma.cpu == allocation && dma.physical == physical);
    assert((uintptr_t)dma.cpu != dma.physical && dma.capacity == 8192);
    assert(dma_buffer_alloc(&dma, 4096, UINT32_MAX) == -16);
    assert(dma_buffer_begin(&dma, DMA_FROM_DEVICE, source, 512) == -22);
    assert(dma_buffer_begin(&dma, DMA_TO_DEVICE, NULL, 512) == -22);
    assert(dma_buffer_begin(&dma, (enum dma_direction)99, NULL, 512) == -22);
    assert(dma_buffer_begin(&dma, DMA_FROM_DEVICE, NULL, 0) == -22);
    assert(dma_buffer_begin(&dma, DMA_TO_DEVICE, source, 8193) == -22);
    assert(dma_buffer_end(&dma, output, true) == -22);

    /* Unaligned CPU buffers need no physical contiguity and never enter a PRDT. */
    assert(dma_buffer_begin(&dma, DMA_TO_DEVICE, source + 1, 8192) == 0);
    assert(!memcmp(dma.cpu, source + 1, 8192));
    assert(dma_buffer_begin(&dma, DMA_FROM_DEVICE, NULL, 512) == -16);
    assert(dma_buffer_free(&dma) == -16);
    assert(dma_buffer_end(&dma, NULL, false) == -16);
    assert(dma.busy && dma.length == 8192 && allocation);
    assert(dma_buffer_end(&dma, output, true) == -22);
    assert(dma.busy);
    assert(dma_buffer_end(&dma, NULL, true) == 0 && !dma.busy);

    assert(dma_buffer_begin(&dma, DMA_FROM_DEVICE, NULL, 8192) == 0);
    memcpy(dma.cpu, source + 1, 8192); /* Device writes its owned physical pages. */
    assert(dma_buffer_end(&dma, output + 1, false) == -16);
    assert(output[1] == 0x9a && dma.busy);
    assert(dma_buffer_end(&dma, output + 1, true) == 0);
    assert(!memcmp(output + 1, source + 1, 8192));
    assert(output[0] == 0x9a && output[8193] == 0x9a);
    memset(output, 0x9a, sizeof(output));
    assert(dma_buffer_begin(&dma, DMA_FROM_DEVICE, NULL, 8192) == 0);
    assert(dma_buffer_end(&dma, NULL, true) == 0); /* Canceled task: discard. */
    assert(output[1] == 0x9a);
    assert(dma_buffer_free(&dma) == 0);
    assert(!dma.cpu && !dma.pages && !dma.physical && !dma.busy);
    assert(dma_buffer_free(&dma) == 0 && allocations == releases);

    /* The same API carries full-width bus addresses without truncation. */
    physical = 0x100120000ull;
    assert(dma_buffer_alloc(&dma, 4096, UINT64_MAX) == 0);
    assert(dma.physical == physical && observed_mask == UINT64_MAX);
    assert(dma_buffer_free(&dma) == 0 && allocations == releases);
    puts("PASS DMA CPU/physical separation, masks, transfer/cancel, STOP retention, rollback");
}

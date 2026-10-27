/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_AHCI_MEMORY_FIXTURE_H
#define RELIEFNT_AHCI_MEMORY_FIXTURE_H
#include <reliefnt/mm.h>
#include <reliefnt/heap.h>
#include <assert.h>
#include <stdlib.h>
/* Independent physical addresses, not truncated host pointers. */
static struct { uint64_t phys; void *cpu; uint32_t pages; } runs[16];
static uint64_t next_physical = 0x100000;
static int allocation_budget = -1;
static unsigned allocated, freed, heap_objects;
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{
    if (!allocation_budget) return 0;
    if (allocation_budget > 0) --allocation_budget;
    uint64_t phys = next_physical;
    if (phys > mask || (uint64_t)pages * 4096 - 1 > mask - phys) return 0;
    next_physical += (uint64_t)pages * 4096;
    for (unsigned i = 0; i < 16; ++i) if (!runs[i].cpu) {
        runs[i].phys = phys; runs[i].pages = pages;
        runs[i].cpu = calloc(pages, 4096); assert(runs[i].cpu);
        ++allocated; return phys;
    }
    abort();
}
void *mm_phys_to_virt(uint64_t physical)
{
    for (unsigned i = 0; i < 16; ++i) if (runs[i].cpu && physical >= runs[i].phys &&
        physical - runs[i].phys < (uint64_t)runs[i].pages * 4096)
        return (unsigned char *)runs[i].cpu + physical - runs[i].phys;
    return NULL;
}
void mm_free_pages(uint64_t physical, uint32_t pages)
{
    for (unsigned i = 0; i < 16; ++i) if (runs[i].cpu && runs[i].phys == physical) {
        assert(runs[i].pages == pages); free(runs[i].cpu); runs[i].cpu = NULL;
        ++freed; return;
    }
    abort();
}
void *kernel_malloc(size_t length)
{
    if (!allocation_budget) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    void *value = malloc(length); if (value) ++heap_objects; return value;
}
void kernel_free(void *value) { assert(heap_objects); --heap_objects; free(value); }

#endif

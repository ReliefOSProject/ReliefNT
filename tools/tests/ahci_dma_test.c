/* SPDX-License-Identifier: Apache-2.0 */
#include "../../drivers/storage/ahci/ahci.h"
#include <reliefnt/mm.h>
#include <reliefnt/heap.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ahci_memory_fixture.h"

int main(void)
{
    struct ahci_hba_port port1 = {0}, port2 = {0};
    struct ahci_port_dma *one, *two;
    for (int budget = 0; budget < 3; ++budget) {
        allocation_budget = budget;
        assert(ahci_dma_create(&port1, UINT32_MAX, &one) == -12);
        assert(!one && !ahci_dma_find(&port1) && !heap_objects && allocated == freed);
    }
    allocation_budget = -1;
    assert(!ahci_dma_create(&port1, UINT32_MAX, &one));
    assert(ahci_dma_find(&port1) == one && ahci_dma_first() == one);
    assert(!ahci_dma_create(&port1, UINT32_MAX, &two) && two == one);
    next_physical = 0x100000000ull;
    assert(ahci_dma_create(&port2, UINT32_MAX, &two) == -12 && !two);
    assert(!ahci_dma_create(&port2, UINT64_MAX, &two) && two != one);
    assert(two->descriptors.physical >= 0x100000000ull);
    assert(!ahci_dma_reset(one, true) && !ahci_dma_reset(two, true));
    for (unsigned which = 0; which < 2; ++which) {
        struct ahci_port_dma *dma = which ? two : one;
        struct ahci_cmd_header *header = ahci_dma_header(dma);
        struct ahci_cmd_table *table = ahci_dma_table(dma);
        uint64_t command = ((uint64_t)header->ctbau << 32) | header->ctba;
        assert(command == dma->descriptors.physical + AHCI_DMA_TABLE_OFFSET);
        assert((command & 127) == 0 && header->prdtl == 1);
        assert((uintptr_t)table == (uintptr_t)dma->descriptors.cpu + AHCI_DMA_TABLE_OFFSET);
        unsigned char *source = malloc(65538), *destination = malloc(65538);
        assert(source && destination);
        memset(source, 0x5a, 65538); memset(destination, 0x9b, 65538);
        assert(!ahci_dma_prepare(dma, 65536, DMA_TO_DEVICE, source + 1, 0x45));
        uint64_t data = ((uint64_t)table->prdt[0].dbau << 32) | table->prdt[0].dba;
        assert(data == dma->data.physical && data != (uintptr_t)(source + 1));
        assert(table->prdt[0].dbc == 65535 && header->flags == 0x45);
        assert(!memcmp(dma->data.cpu, source + 1, 65536));
        assert(ahci_dma_prepare(dma, 512, DMA_FROM_DEVICE, NULL, 5) == -16);
        assert(ahci_dma_reset(dma, false) == -16 && dma->data.busy);
        assert(ahci_dma_destroy(dma, false) == -16 && ahci_dma_find(dma->port) == dma);
        assert(!ahci_dma_reset(dma, true) && !dma->data.busy);
        assert(!ahci_dma_prepare(dma, 65536, DMA_FROM_DEVICE, NULL, 5));
        memcpy(dma->data.cpu, source + 1, 65536);
        assert(!dma_buffer_end(&dma->data, destination + 1, true));
        assert(!memcmp(destination + 1, source + 1, 65536));
        assert(destination[0] == 0x9b && destination[65537] == 0x9b);
        free(source); free(destination);
    }
    assert(ahci_dma_header(one) != ahci_dma_header(two));
    assert(!ahci_dma_destroy(one, true) && !ahci_dma_find(&port1));
    assert(!ahci_dma_destroy(two, true) && !ahci_dma_first());
    assert(!heap_objects && allocated == freed);
    puts("PASS actual AHCI per-port PRDT/CTBA64, high CPU buffers, STOP/OOM retention");
}

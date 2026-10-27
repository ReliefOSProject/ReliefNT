/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/console.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ahci_memory_fixture.h"
#include "../../drivers/storage/nvme/storage_nvme.c"

uint64_t mm_alloc_pages(uint32_t count) { return mm_alloc_pages_below(count, UINT64_MAX); }
bool paging_kernel_direct_map_range(uint64_t physical, uint64_t length)
{
    void *start = mm_phys_to_virt(physical);
    return start && length && (uintptr_t)mm_phys_to_virt(physical + length - 1) ==
        (uintptr_t)start + length - 1;
}
void *paging_kernel_direct_map(uint64_t physical) { return mm_phys_to_virt(physical); }
void storage_memzero(void *memory, size_t size) { memset(memory, 0, size); }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static void *expected_mmio;
static unsigned unmaps;
uint16_t pci_config_read16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{ (void)bus;(void)slot;(void)function;assert(offset==4);return 6; }
void pci_config_write16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{ (void)bus;(void)slot;(void)function;assert(offset==4 && !(value&4)); }
void pci_unmap_mmio(void *pointer,uint64_t bytes)
{ assert(pointer==expected_mmio && bytes==NVME_MMIO_BYTES && !g_nvme_controllers[0].dma_active);++unmaps; }
int main(void)
{
    unsigned char registers[0x4000] __attribute__((aligned(8))) = {0};
    struct nvme_controller *controller = &g_nvme_controllers[0];
    controller->mmio = registers;expected_mmio=registers;
    /* A previous disable wrote CC.EN=0, but RDY is still 1: DMA can still run. */
    nvme_write32(controller, NVME_REG_CC, 0);
    nvme_write32(controller, NVME_REG_CSTS, NVME_CSTS_RDY);
    assert(nvme_disable_controller(controller) == -5);

    next_physical = 0x100020000ull;
    assert(!nvme_allocate_pages(NVME_IO_BUFFER_PAGES, &controller->io_buffer_phys,
                               &controller->io_buffer));
    controller->io_buffer_pages = NVME_IO_BUFFER_PAGES;
    assert(!nvme_allocate_page(&controller->prp_list_phys,
                               (uint8_t **)(void *)&controller->prp_list));
    struct nvme_command command = {0};
    for (uint32_t count = 1; count <= NVME_IO_BUFFER_PAGES; ++count) {
        assert(!nvme_prepare_prp(controller, count * 4096, &command));
        assert(command.prp1 == controller->io_buffer_phys && command.prp1 > UINT32_MAX);
        if (count == 1) assert(!command.prp2);
        else if (count == 2) assert(command.prp2 == command.prp1 + 4096);
        else {
            assert(command.prp2 == controller->prp_list_phys && command.prp2 > UINT32_MAX);
            for (uint32_t page = 1; page < count; ++page)
                assert(controller->prp_list[page - 1] == command.prp1 + (uint64_t)page * 4096);
        }
    }
    assert(nvme_prepare_prp(controller, (NVME_IO_BUFFER_PAGES + 1u) * 4096, &command) == -22);
    unsigned before = freed;
    assert(nvme_controller_stop(controller) == -5);
    assert(freed == before && controller->io_buffer && controller->prp_list && !unmaps);
    /* CC.EN is already zero on the second STOP: RDY remains authoritative. */
    assert(nvme_controller_stop(controller) == -5 && freed == before);
    nvme_write32(controller, NVME_REG_CSTS, NVME_CSTS_CFS); /* Fatal but now RDY=0. */
    assert(!nvme_controller_stop(controller));
    nvme_controller_release(controller);
    assert(!controller->mmio && !controller->io_buffer && !controller->prp_list);
    assert(allocated == freed && !heap_objects && unmaps==1);
    assert(!nvme_controller_stop(controller));
    puts("PASS native NVMe PRP64/cross-page bounds and repeated failed STOP retention");
}

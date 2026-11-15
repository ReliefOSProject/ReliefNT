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
static bool asynchronous;
bool storage_async_can_yield(void) { return asynchronous; }
uint64_t time_ticks(void) { return 1; }
uint32_t sched_current_pid(void) { return 7; }
void storage_memcpy(void *out, const void *in, size_t bytes) { memcpy(out,in,bytes); }
void kernel_spin_lock(struct kernel_spinlock *lock) { assert(!lock->state);lock->state=1; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { assert(lock->state);lock->state=0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ *flags=0;kernel_spin_lock(lock); }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ (void)flags;kernel_spin_unlock(lock); }
static unsigned notifications;
void storage_completion_notify(void) { ++notifications; }
void storage_wait_hint(uint64_t deadline) { assert(deadline==501); }
static void (*irq_handler)(void *);
static void *irq_context;
static unsigned irq_frees;
int pci_read_device(uint8_t bus,uint8_t slot,uint8_t function,struct pci_device *out)
{ *out=(struct pci_device){.bus=bus,.slot=slot,.function=function,.vendor_id=0x1b36,.device_id=0x10};return 0; }
int pci_request_msix_irq(const struct reliefos_driver_pci_device *dev,void (*fn)(void *),void *arg,uint32_t *out)
{ assert(dev->vendor_id==0x1b36 && dev->device_id==0x10);irq_handler=fn;irq_context=arg;*out=1;return 0; }
int pci_request_irq(const struct reliefos_driver_pci_device *dev,void (*fn)(void *),void *arg,uint32_t *out)
{ (void)dev;(void)fn;(void)arg;(void)out;assert(!"unexpected fallback");return -95; }
void pci_free_irq(uint32_t handle) { assert(handle==1);++irq_frees; }

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
    /* A read awaiting hardware must release the caller with retry ownership,
     * without spinning until timeout or writing into the caller's old buffer. */
    struct nvme_command sq[32]={0};
    struct nvme_completion cq[32]={0};
    unsigned char destination[512];memset(destination,0x5a,sizeof(destination));
    controller->io_sq=sq;controller->io_cq=cq;controller->queue_depth=32;
    controller->cq_phase_io=1;controller->ready=1;controller->queues_live=true;
    controller->doorbell_stride=4;asynchronous=true;
    nvme_interrupt_prepare(controller);
    assert(nvme_readwrite_once(controller,1,64,1,destination,0)==-11);
    assert(destination[0]==0x5a && controller->io_sq_tail==1);
    assert(nvme_readwrite_once(controller,1,64,1,destination,0)==-11);
    assert(controller->io_sq_tail==1); /* No duplicate read on syscall retry. */
    memset(controller->io_buffer,0xa7,512);
    cq[0]=(struct nvme_completion){.sq_id=1,.command_id=sq[0].cdw0>>16,.status=1};
    irq_handler(irq_context);
    assert(notifications==1 && controller->io_cq_head==1 && destination[0]==0x5a);
    assert(!nvme_readwrite_once(controller,1,64,1,destination,0));
    assert(destination[0]==0xa7 && !controller->pending_read.active);
    irq_handler(irq_context);assert(notifications==1); /* Stale IRQ is harmless. */
    /* Queue phase rollover uses the same completion path in IRQ and polling. */
    for(unsigned i=0;i<33;++i) {
        struct nvme_command next={.cdw0=NVME_IO_READ,.nsid=1};
        assert(!nvme_queue_issue(controller,&next));
        unsigned head=controller->io_cq_head;
        cq[head]=(struct nvme_completion){.sq_id=1,.command_id=next.cdw0>>16,
            .status=controller->cq_phase_io};
        irq_handler(irq_context);
        assert(!nvme_queue_poll(controller));
    }
    assert(controller->cq_phase_io==0 && controller->io_cq_head==2);
    /* A changed retry buffer never receives the previous read's stale bytes. */
    unsigned char replacement[512];memset(replacement,0x39,sizeof(replacement));
    assert(nvme_readwrite_once(controller,1,65,1,destination,0)==-11);
    unsigned head=controller->io_cq_head;
    cq[head]=(struct nvme_completion){.sq_id=1,.command_id=controller->completion_cid,
        .status=controller->cq_phase_io};
    irq_handler(irq_context);
    assert(nvme_readwrite_once(controller,1,66,1,replacement,0)==-11);
    assert(destination[0]==0xa7 && replacement[0]==0x39);
    head=controller->io_cq_head;
    cq[head]=(struct nvme_completion){.sq_id=1,.command_id=controller->completion_cid,
        .status=controller->cq_phase_io};
    irq_handler(irq_context);
    struct nvme_namespace ns={.controller=controller,.nsid=1};
    nvme_block_cancel(&ns,7);
    assert(!controller->pending_read.active && replacement[0]==0x39);
    struct nvme_command bad={.cdw0=NVME_IO_READ,.nsid=1};
    assert(!nvme_queue_issue(controller,&bad));head=controller->io_cq_head;
    cq[head]=(struct nvme_completion){.sq_id=1,.command_id=controller->completion_cid+1,
        .status=controller->cq_phase_io};
    irq_handler(irq_context);
    assert(nvme_queue_poll(controller)==-5 && controller->queue_fault);
    /* Fixture retirement: the following cases exercise STOP, not completion. */
    controller->io_sq=NULL;controller->io_cq=NULL;controller->ready=0;
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
    assert(irq_frees==1);
    assert(!nvme_controller_stop(controller));
    puts("PASS NVMe PRP64, async resume, MSI-X CQ/phase/CID, stale buffer discard, cancel and failed STOP retention");
}

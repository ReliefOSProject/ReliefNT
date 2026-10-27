/* Exercise the actual ES1371 transport; only its port registers are simulated. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefos/driver.h>
static uint32_t regs[64];
static uint8_t dma_memory[65536];
static unsigned dma_allocations, dma_frees;
static int teardown_guard, dma_stuck, teardown_error;
static uint16_t pci_command;
static uint64_t high_pages(uint32_t pages)
{ (void)pages; return 0x100000000ULL; }
static void free_pages(uint64_t phys, uint32_t pages)
{ (void)phys; (void)pages; }
static uint64_t alloc_dma(uint32_t pages, uint64_t mask)
{ assert(pages == 4 && mask == 0xffffffffULL); ++dma_allocations; return 0x100000; }
static void free_dma(uint64_t phys, uint32_t pages)
{
    assert(!teardown_guard || !(pci_command & 4u));
    assert(phys == 0x100000 && pages == 4); ++dma_frees;
}
static void *map_dma(uint64_t phys)
{ assert(phys == 0x100000); return dma_memory; }
static uint32_t read_long(uint16_t port)
{ return regs[port / 4U] | ((dma_stuck && port == 0x40) ? 0x40u : 0); }
static void write_long(uint16_t port, uint32_t value) { regs[port / 4U] = value; }
static void log_text(const char *text) { (void)text; }
static void log_record(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
static uint16_t read_pci(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{ (void)bus; (void)slot; (void)function; assert(offset == 4); return pci_command; }
static void write_pci(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value)
{ (void)bus; (void)slot; (void)function; assert(offset == 4); pci_command = value; }
static void report_teardown(int error) { teardown_error = error; }
#ifndef ES1371_QUEUE_SOURCE
#define ES1371_QUEUE_SOURCE "../../drivers/sound/es1371/es1371_main.c"
#endif
#include ES1371_QUEUE_SOURCE
static void cursor(uint32_t offset) { regs[ES_REG_DAC1_SIZE / 4U] = offset << 14; }
static int all_zero(const uint8_t *p, unsigned n)
{ for(unsigned i=0;i<n;i++)if(p[i])return 0;return 1; }
int main(void)
{
    struct reliefos_driver_kernel_api api = {.inl=read_long, .outl=write_long,
        .console_write=log_text,.console_log=log_record};
    uint8_t ring[ES1371_DMA_BYTES] = {0}, pcm[2048];
    memset(pcm, 0x5a, sizeof pcm);
    kernel_api = &api;es1371.dma_buffer=ring;es1371.dma_configured=1;
    es1371.active=es1371.present=1;es1371.sample_rate=48000;
    es1371.dma_last_cursor=ES1371_DMA_BYTES-2048U;
    es1371_recover_empty(es1371.dma_last_cursor);
    struct reliefos_audio_state state;
    es1371_get_state(&state);
    assert(state.queued_bytes==0); /* Idle lead is never application backlog. */
    assert(!es1371_queue_block(pcm,sizeof pcm));
    es1371_get_state(&state);assert(state.queued_bytes==sizeof pcm);
    es1371.dma_running=1;
    cursor(0);es1371_get_state(&state);assert(state.queued_bytes==sizeof pcm);
    cursor(3072);es1371_get_state(&state);assert(state.queued_bytes==1024);
    /* The consumed half must be silent when the cyclic DMA ring revisits it. */
    assert(all_zero(ring+2048,1024));
    assert(!all_zero(ring+3072,1024));
    cursor(4096);es1371_get_state(&state);assert(!state.queued_bytes);
    assert(all_zero(ring,sizeof ring));
    cursor(8192);es1371_get_state(&state);assert(!state.queued_bytes);
    assert(all_zero(ring,sizeof ring));
    puts("actual_ES1371_idle_lead_excluded_partial_wrap_drain_and_no_stale_PCM_replay PASS");
    api.alloc_pages = high_pages; api.free_pages = free_pages;
    api.alloc_dma = alloc_dma; api.free_dma = free_dma;
    api.phys_to_virt = map_dma;
    memset(&es1371, 0, sizeof(es1371));
    assert(!es1371_alloc_dma());
    assert(dma_allocations == 1 && !dma_frees && es1371.dma_phys == 0x100000 &&
           es1371.dma_buffer == dma_memory);
    api.free_dma(es1371.dma_phys, ES1371_DMA_PAGES);
    assert(dma_frees == 1);
    puts("actual_ES1371_DMA_allocation_uses_32bit_pool_above_4GiB_RAM PASS");
    assert(!es1371_alloc_dma()); dma_frees = 0;
    api.pci_read16 = read_pci; api.pci_write16 = write_pci;
    api.report_teardown_failure = report_teardown;
    es1371.io_port = 0x40; es1371.present = es1371.active = 1;
    pci_command = 5; teardown_guard = dma_stuck = 1;
    es1371_driver_fini();
    assert(teardown_error == -110 && !dma_frees && es1371.dma_phys == 0x100000 &&
           es1371.dma_buffer == dma_memory);
    dma_stuck = 0; teardown_error = 0;
    es1371_driver_fini();
    assert(!teardown_error && dma_frees == 1 && !(pci_command & 4u) &&
           !es1371.dma_phys && !es1371.dma_buffer);
    puts("actual_ES1371_failed_STOP_retains_DMA_then_retry_disables_bus_master_before_release PASS");
    return 0;
}

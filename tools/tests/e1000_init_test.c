#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../drivers/net/e1000/e1000.c"

static unsigned allocations, allocation_limit = 1024;
static uint64_t next_physical = 0x100000ULL;
#define TEST_E1000_BAR_PHYS 0xfebc0000ULL
#define TEST_E1000_REGISTER_BYTES 0x5408ULL
static struct { uint64_t phys; uint32_t pages; void *host; } allocation_map[1024];
static int registration_error;
static int fail_mmio_map;
static unsigned mmio_map_calls, mmio_maps, mmio_unmaps;
static uint16_t command_bits;
static uint8_t *registers;
static uint64_t allocate(uint32_t count)
{
    if (allocations == allocation_limit) return 0;
    void *p = mmap(NULL, count * 4096u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    uint32_t slot;
    for (slot = 0; slot < 1024; ++slot) if (!allocation_map[slot].phys) break;
    assert(slot < 1024 && next_physical <= UINT64_MAX - (uint64_t)count * 4096U);
    allocation_map[slot].phys = next_physical;
    allocation_map[slot].pages = count;
    allocation_map[slot].host = p;
    next_physical += (uint64_t)count * 4096U;
    ++allocations;
    return allocation_map[slot].phys;
}
static void *physical_alias(uint64_t address)
{
    for (uint32_t slot = 0; slot < 1024; ++slot) {
        uint64_t bytes = (uint64_t)allocation_map[slot].pages * 4096U;
        if (allocation_map[slot].phys && address >= allocation_map[slot].phys &&
            address - allocation_map[slot].phys < bytes)
            return (uint8_t *)allocation_map[slot].host +
                   (address - allocation_map[slot].phys);
    }
    return NULL;
}
static void release(uint64_t p, uint32_t count)
{
    assert(allocations);
    for (uint32_t slot = 0; slot < 1024; ++slot) if (allocation_map[slot].phys == p) {
        assert(allocation_map[slot].pages == count);
        assert(munmap(allocation_map[slot].host, count * 4096u) == 0);
        memset(&allocation_map[slot], 0, sizeof(allocation_map[slot]));
        --allocations;
        return;
    }
    assert(!"unknown or duplicate physical free");
}
static int find(uint16_t vendor, uint16_t device, struct reliefos_driver_pci_device *out)
{
    if (vendor != 0x8086 || device != 0x100f) return -1;
    *out = (struct reliefos_driver_pci_device){.vendor_id = vendor, .device_id = device, .class_code = 2};
    return 0;
}
static uint32_t read32(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ (void)b; (void)s; (void)f; return offset == 0x10 ? (uint32_t)TEST_E1000_BAR_PHYS : 0; }
static uint16_t read16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ (void)b; (void)s; (void)f; (void)offset; return command_bits; }
static void write16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset, uint16_t value)
{ (void)b; (void)s; (void)f; (void)offset; command_bits = value; }
static void *map_mmio(uint64_t phys, uint64_t bytes)
{
    assert(phys == TEST_E1000_BAR_PHYS);
    assert(bytes == TEST_E1000_REGISTER_BYTES);
    assert(!(command_bits & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_BUS_MASTER)));
    ++mmio_map_calls;
    if (fail_mmio_map) return NULL;
    ++mmio_maps;
    return registers;
}
static void unmap_mmio(void *pointer, uint64_t bytes)
{
    assert(pointer == registers && bytes == TEST_E1000_REGISTER_BYTES);
    assert(!(command_bits & PCI_COMMAND_BUS_MASTER));
    assert(mmio_maps > mmio_unmaps);
    ++mmio_unmaps;
}
static void pause_ms(uint64_t ms)
{ (void)ms; *(uint32_t *)(registers + E1000_REG_CTRL) &= ~(1u << 26); }
static void log_text(const char *text) { (void)text; }
static void log_record(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
static int register_ops(const struct reliefos_driver_e1000_ops *ops)
{ assert(ops->is_ready()); return registration_error; }

int main(void)
{
    registers = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(registers != MAP_FAILED);
    *(uint32_t *)(registers + E1000_REG_STATUS) = 1u << 1;
    *(uint32_t *)(registers + E1000_REG_RAL) = 0x06290c00;
    *(uint32_t *)(registers + E1000_REG_RAH) = E1000_RAH_AV | 0x9729;
    const struct reliefos_driver_kernel_api api = {
        .abi_version = RELIEFOS_DRIVER_ABI_VERSION, .struct_size = sizeof(api),
        .alloc_pages = allocate, .free_pages = release, .console_write = log_text,
        .console_log = log_record, .phys_to_virt = physical_alias,
        .map_mmio = map_mmio, .unmap_mmio = unmap_mmio,
        .pci_find = find, .pci_read16 = read16, .pci_write16 = write16,
        .pci_read32 = read32, .sleep_ms = pause_ms, .register_e1000 = register_ops,
    };
    struct reliefos_driver_kernel_api old_api = api;
    old_api.abi_version = 1U;
    assert(e1000_driver_init(&old_api) < 0);
    assert(allocations == 0);
    struct reliefos_driver_kernel_api missing_alias_api = api;
    missing_alias_api.phys_to_virt = NULL;
    assert(e1000_driver_init(&missing_alias_api) < 0);
    assert(allocations == 0);
    struct reliefos_driver_kernel_api missing_mmio_api = api;
    missing_mmio_api.map_mmio = NULL;
    assert(e1000_driver_init(&missing_mmio_api) < 0);
    assert(allocations == 0 && mmio_map_calls == 0);
    struct reliefos_driver_kernel_api missing_unmap_api = api;
    missing_unmap_api.unmap_mmio = NULL;
    assert(e1000_driver_init(&missing_unmap_api) < 0);
    assert(allocations == 0 && mmio_map_calls == 0);
    fail_mmio_map = 1;
    command_bits = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    assert(e1000_driver_init(&api) < 0);
    assert(allocations == 0 && mmio_map_calls == 1 && mmio_maps == 0 && mmio_unmaps == 0);
    assert(command_bits == (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER));
    fail_mmio_map = 0;
    allocation_limit = 1;
    assert(e1000_driver_init(&api) < 0);
    assert(allocations == 0 && mmio_maps == mmio_unmaps);
    allocation_limit = 100;
    if (E1000_RX_COUNT > allocation_limit) {
        assert(e1000_driver_init(&api) < 0);
        assert(allocations == 0);
    }
    allocation_limit = 1024;
    registration_error = -16;
    assert(e1000_driver_init(&api) == -16);
    assert(allocations == 0 && !g_e1000.active && mmio_maps == mmio_unmaps);
    registration_error = 0;
    assert(e1000_driver_init(&api) == 0);
    assert(g_e1000.mmio_base == (uintptr_t)registers);
    assert(g_e1000.mmio_base != TEST_E1000_BAR_PHYS);
    assert(mmio_maps == mmio_unmaps + 1);
    assert(command_bits & PCI_COMMAND_BUS_MASTER);
    assert(*(uint32_t *)(registers + E1000_REG_RDBAL) == (uint32_t)g_e1000.rx_phys);
    assert(g_e1000.rx[0].addr == g_e1000.rx_buf_phys[0]);
    assert(*(uint32_t *)(registers + E1000_REG_CTRL) & (1u << 6));
    assert(!memcmp(g_e1000.mac, "\x00\x0c\x29\x06\x29\x97", 6));
    *(uint32_t *)(registers + E1000_REG_STATUS) = 0;
    struct module_e1000_info info;
    e1000_get_info(&info);
    assert(info.present && !info.active && !e1000_is_ready());
    *(uint32_t *)(registers + E1000_REG_STATUS) = 1u << 1;
    assert(e1000_is_ready());
    unsigned char frame[2048];
    uint32_t length;
    g_e1000.rx[0].length = 65535;
    g_e1000.rx[0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
    assert(e1000_poll(frame, sizeof(frame), &length) == 0 && length == 0);
    /* A full TCP window arrives before the next poll, including ring wrap. */
    for (unsigned burst = 0; burst < 5; ++burst) {
        unsigned queued = 0;
        for (unsigned packet = 0; packet < 64; ++packet) {
            unsigned index = (g_e1000.rx_index + packet) % E1000_RX_COUNT;
            if (g_e1000.rx[index].status & E1000_RX_STATUS_DD) break;
            memset(g_e1000.rx_buf[index], packet, 1460);
            g_e1000.rx[index].length = 1460;
            g_e1000.rx[index].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
            ++queued;
        }
        assert(queued == 64);
        for (unsigned packet = 0; packet < queued; ++packet) {
            assert(e1000_poll(frame, sizeof(frame), &length) == 1 && length == 1460);
            for (unsigned byte = 0; byte < length; ++byte) assert(frame[byte] == packet);
        }
        assert(e1000_poll(frame, sizeof(frame), &length) == 0 && length == 0);
    }
    e1000_driver_fini();
    assert(!allocations && !g_e1000.active && mmio_maps == mmio_unmaps);
    assert(command_bits == (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER));
    assert(munmap(registers, 0x20000) == 0);
    puts("PASS e1000 initialization, VMware MAC, link setup, RX bounds and failure cleanup");
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Separate production objects, with only the PCI/MMIO/DMA boundary replaced. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/net/r8169/internal.h"

extern const struct reliefos_driver_module r8169_driver_module;

static uint8_t registers[0x10000];
static uint16_t mac_ocp[32768], phy_ocp[32768], ephy[128], phy[32];
static uint32_t csi[1024];
static uint16_t device_id, pci_command, original_command;
static uint16_t pmcsr;
static bool power_capability;
static unsigned live, maps, unmaps, allocation_attempts, allocation_failure;
static unsigned kicks, resets, mdio_accesses;
static int reset_stuck, indirect_stuck, mapping_failure, registration_error, teardown_error;
static uint64_t next_physical = 0x140000000ULL;
static const uint64_t bar = 0xc000000000ULL;
static const struct reliefos_driver_e1000_ops *network;
static struct allocation { uint64_t physical; unsigned pages; void *bytes; } memory[8];

static uint32_t raw32(unsigned offset)
{
    uint32_t value;
    assert(offset + 4 <= sizeof(registers));
    memcpy(&value, registers + offset, 4);
    return value;
}

static void store32(unsigned offset, uint32_t value)
{
    assert(offset + 4 <= sizeof(registers));
    memcpy(registers + offset, &value, 4);
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    assert(!lock->state);
    lock->state = 1;
    *flags = 0;
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(lock->state);
    lock->state = 0;
}

uint8_t rtl_read8(const struct rtl_device *d, unsigned offset)
{
    assert((d->registers == registers || d->io_base == 0xe000) && offset < d->mmio_bytes);
    if (offset == 0x37 && !reset_stuck)
        registers[offset] = (registers[offset] & ~0x90u) | 1;
    return registers[offset];
}

uint16_t rtl_read16(const struct rtl_device *d, unsigned offset)
{
    assert((d->registers == registers || d->io_base == 0xe000) &&
           !(offset & 1) && offset + 2 <= d->mmio_bytes);
    uint16_t value;
    memcpy(&value, registers + offset, 2);
    return value;
}

uint32_t rtl_read32(const struct rtl_device *d, unsigned offset)
{
    assert((d->registers == registers || d->io_base == 0xe000) &&
           !(offset & 3) && offset + 4 <= d->mmio_bytes);
    if (offset == 0x40)
        return raw32(offset) | 0x800; /* No outstanding TX DMA in the boundary model. */
    return raw32(offset);
}

void rtl_write8(struct rtl_device *d, unsigned offset, uint8_t value)
{
    assert((d->registers == registers || d->io_base == 0xe000) && offset < d->mmio_bytes);
    if (offset == 0x37 && (value & 0x10))
        ++resets;
    if (offset == 0x38 && device_id == 0x8168) {
        assert(value == 0x40);
        ++kicks;
        return;
    }
    registers[offset] = value;
}

void rtl_write16(struct rtl_device *d, unsigned offset, uint16_t value)
{
    assert((d->registers == registers || d->io_base == 0xe000) &&
           !(offset & 1) && offset + 2 <= d->mmio_bytes);
    if (offset == 0x90 && device_id == 0x8125)
        ++kicks;
    memcpy(registers + offset, &value, 2);
}

void rtl_write32(struct rtl_device *d, unsigned offset, uint32_t value)
{
    assert((d->registers == registers || d->io_base == 0xe000) &&
           !(offset & 3) && offset + 4 <= d->mmio_bytes);
    unsigned reg = (value >> 16) & 0x7fff;
    uint16_t data = value;
    store32(offset, value);
    if (offset == 0xb0) {
        if (value & 0x80000000u)
            mac_ocp[reg] = data;
        else
            store32(offset, mac_ocp[reg]);
    } else if (offset == 0xb8) {
        ++mdio_accesses;
        if (indirect_stuck)
            return;
        if (value & 0x80000000u) {
            phy_ocp[reg] = data;
            if (reg == 0xa400 / 2)
                phy_ocp[reg] &= ~0x8000u;
            if (reg == 0xb820 / 2) {
                if (data & 0x10)
                    phy_ocp[0xb800 / 2] |= 0x40;
                else
                    phy_ocp[0xb800 / 2] &= ~0x40u;
            }
            store32(offset, value & ~0x80000000u);
        } else {
            store32(offset, 0x80000000u | phy_ocp[reg]);
        }
    } else if (offset == 0x60) {
        ++mdio_accesses;
        if (indirect_stuck)
            return;
        reg &= 31;
        if (value & 0x80000000u) {
            phy[reg] = data;
            if (reg == 0)
                phy[reg] &= ~0x8000u;
            store32(offset, value & ~0x80000000u);
        } else {
            store32(offset, 0x80000000u | phy[reg]);
        }
    } else if (offset == 0x80) {
        if (indirect_stuck)
            return;
        reg &= 127;
        if (value & 0x80000000u) {
            ephy[reg] = data;
            store32(offset, value & ~0x80000000u);
        } else {
            store32(offset, 0x80000000u | ephy[reg]);
        }
    } else if (offset == 0x68) {
        if (indirect_stuck)
            return;
        reg = (value & 0xfff) / 4;
        if (value & 0x80000000u) {
            csi[reg] = raw32(0x64);
            store32(offset, value & ~0x80000000u);
        } else {
            store32(0x64, csi[reg]);
            store32(offset, value | 0x80000000u);
        }
    }
}

void rtl_delay_us(struct rtl_device *d, unsigned us)
{
    (void)d;
    (void)us;
}

static uint64_t allocate(uint32_t pages, uint64_t mask)
{
    ++allocation_attempts;
    if (allocation_attempts == allocation_failure)
        return 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].physical)
            continue;
        void *bytes = calloc(pages, 4096);
        assert(bytes);
        uint64_t physical = next_physical;
        next_physical += (uint64_t)pages * 4096;
        assert(physical + (uint64_t)pages * 4096 - 1 <= mask);
        memory[i] = (struct allocation){physical, pages, bytes};
        ++live;
        return physical;
    }
    abort();
}

static void release(uint64_t physical, uint32_t pages)
{
    assert(!(pci_command & 4));
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].physical != physical)
            continue;
        assert(memory[i].pages == pages);
        free(memory[i].bytes);
        memory[i] = (struct allocation){0};
        --live;
        return;
    }
    abort();
}

static void *alias(uint64_t physical)
{
    for (unsigned i = 0; i < 8; ++i)
        if (memory[i].physical && physical >= memory[i].physical &&
            physical - memory[i].physical < (uint64_t)memory[i].pages * 4096)
            return (uint8_t *)memory[i].bytes + physical - memory[i].physical;
    return NULL;
}

static int find(uint16_t vendor, uint16_t id, struct reliefos_driver_pci_device *out)
{
    if (vendor != 0x10ec || id != device_id)
        return -19;
    *out = (struct reliefos_driver_pci_device){.vendor_id = vendor, .device_id = id,
        .bus = 2, .slot = 3, .class_code = 2};
    return 0;
}

static uint16_t pci_read16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{
    assert(b == 2 && s == 3 && f == 0);
    if (power_capability) {
        if (offset == 6)
            return 0x10;
        if (offset == 0x40)
            return 1;
        if (offset == 0x44)
            return pmcsr;
    }
    return offset == 4 ? pci_command : 0;
}

static void pci_write16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset, uint16_t value)
{
    assert(b == 2 && s == 3 && f == 0);
    if (power_capability && offset == 0x44) {
        pmcsr = (value & ~0x8000u) | (pmcsr & ~value & 0x8000u);
        return;
    }
    assert(offset == 4);
    pci_command = value;
}

static uint32_t pci_read32(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{
    assert(b == 2 && s == 3 && f == 0);
    if (power_capability && offset == 0x34)
        return 0x40;
    if (offset == 0x10)
        return 0xe001;
    if (offset == 0x18)
        return (uint32_t)bar | 4;
    if (offset == 0x1c)
        return bar >> 32;
    return 0;
}

static void *map(uint64_t physical, uint64_t bytes)
{
    assert(physical == bar && bytes <= sizeof(registers) && !(pci_command & 7));
    if (mapping_failure)
        return NULL;
    ++maps;
    return registers;
}

static void unmap(void *pointer, uint64_t bytes)
{
    assert(pointer == registers && bytes <= sizeof(registers));
    assert(!(pci_command & 4) && !live && maps == unmaps + 1);
    ++unmaps;
}

static void sleep_ms(uint64_t milliseconds)
{
    (void)milliseconds;
}

static uint8_t io_inb(uint16_t port)
{
    assert(port >= 0xe000 && port < 0xe100);
    return registers[port - 0xe000];
}

static void io_outb(uint16_t port, uint8_t value)
{
    assert(port >= 0xe000 && port < 0xe100);
    registers[port - 0xe000] = value;
}

static uint32_t io_inl(uint16_t port)
{
    assert(port >= 0xe000 && port <= 0xe0fc);
    return raw32(port - 0xe000);
}

static void io_outl(uint16_t port, uint32_t value)
{
    assert(port >= 0xe000 && port <= 0xe0fc);
    store32(port - 0xe000, value);
}

static int register_network(const struct reliefos_driver_e1000_ops *ops)
{
    if (registration_error)
        return registration_error;
    assert(!network);
    network = ops;
    return 0;
}

static void log_message(enum console_log_level level, const char *format, ...)
{
    (void)level;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static void failed_stop(int error)
{
    teardown_error = error;
}

static const struct reliefos_driver_kernel_api api = {
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION, .struct_size = sizeof(api),
    .pci_find = find, .pci_read16 = pci_read16, .pci_write16 = pci_write16,
    .pci_read32 = pci_read32, .map_mmio = map, .unmap_mmio = unmap,
    .alloc_dma = allocate, .free_dma = release, .phys_to_virt = alias,
    .sleep_ms = sleep_ms, .console_log = log_message,
    .register_e1000 = register_network, .report_teardown_failure = failed_stop,
};

static void fixture(uint16_t id, uint32_t xid)
{
    assert(!live && maps == unmaps);
    memset(registers, 0, sizeof(registers));
    memset(mac_ocp, 0, sizeof(mac_ocp));
    memset(phy_ocp, 0, sizeof(phy_ocp));
    memset(phy, 0, sizeof(phy));
    memset(ephy, 0, sizeof(ephy));
    memset(csi, 0, sizeof(csi));
    device_id = id;
    pci_command = original_command = 7;
    power_capability = false;
    pmcsr = 0;
    allocation_attempts = allocation_failure = kicks = resets = mdio_accesses = 0;
    reset_stuck = indirect_stuck = mapping_failure = registration_error = teardown_error = 0;
    network = NULL;
    store32(0x40, xid);
    store32(0, 0x33221102);
    store32(4, 0x5544);
    store32(0x19e0, 0x33221102);
    store32(0x19e4, 0x5544);
    registers[0x6c] = 2;
    registers[0xd3] = 0x32;
    registers[0xe2] = 3;
    registers[0xe3] = 1;
    phy[2] = 0x001c;
    phy[3] = 0xc916;
    mac_ocp[0xe00e / 2] = 0x2000;
    phy_ocp[0xa420 / 2] = 3;
}

static void finish(void)
{
    network = NULL;
    r8169_driver_module.fini();
    uint16_t expected = device_id ? original_command & ~4u : original_command;
    assert(!teardown_error && !live && maps == unmaps && pci_command == expected);
}

static void ring_tests(enum rtl_family family)
{
    fixture(family == RTL_FAMILY_8125 ? 0x8125 : 0x8168, 0);
    struct rtl_device d = {.api = &api, .registers = registers,
        .family = family, .active = true, .mmio_bytes = sizeof(registers)};
    assert(!rtl_allocate_rings(&d) && live == 4);
    assert(d.rx_physical > UINT32_MAX && d.rx[0].address > UINT32_MAX);
    assert(d.rx[RTL_RX_COUNT - 1].command & RTL_DESC_EOR);
    uint8_t input[RTL_MAX_FRAME], output[RTL_MAX_FRAME + 1];
    memset(input, 0xa5, sizeof(input));
    uint32_t length = 99;
    assert(rtl_receive(&d, output, sizeof(output), &length) == 0 && !length);
    assert(rtl_send(&d, NULL, 60) == -22);
    assert(rtl_send(&d, input, 13) == -22);
    assert(rtl_send(&d, input, RTL_MAX_FRAME + 1) == -90);
    registers[0x6c] = 0;
    assert(rtl_send(&d, input, 60) == -100);
    registers[0x6c] = 2;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        for (unsigned i = 0; i < RTL_TX_COUNT - 1; ++i)
            assert(rtl_send(&d, input, i == 0 ? 14 : sizeof(input)) == 0);
        assert(rtl_send(&d, input, 60) == -11);
        for (unsigned i = 0; i < RTL_TX_COUNT; ++i) {
            uint32_t command = d.tx[i].command;
            if (!(command & RTL_DESC_OWN))
                continue;
            assert((command & (RTL_DESC_FIRST | RTL_DESC_LAST)) ==
                   (RTL_DESC_FIRST | RTL_DESC_LAST));
            unsigned bytes = command & 0x3fff;
            assert(bytes == 60 || bytes == sizeof(input));
            if (bytes == 60)
                for (unsigned j = 14; j < bytes; ++j)
                    assert(d.tx_data[i * RTL_BUFFER_SIZE + j] == 0);
            assert((command & RTL_DESC_EOR) == (i == RTL_TX_COUNT - 1 ? RTL_DESC_EOR : 0));
            d.tx[i].command &= ~RTL_DESC_OWN;
        }
    }
    assert(kicks == 4 * (RTL_TX_COUNT - 1));
    for (unsigned i = 0; i < RTL_RX_COUNT * 2; ++i) {
        unsigned index = d.rx_next;
        memcpy(d.rx_data + index * RTL_BUFFER_SIZE, input, sizeof(input));
        memset(output, 0, sizeof(output));
        /* Reserved classic status bits are not an RX error indication. */
        d.rx[index].command = RTL_DESC_FIRST | RTL_DESC_LAST | (1u << 23) |
            (1u << 24) | (sizeof(input) + 4);
        assert(rtl_receive(&d, output, sizeof(output), &length) == 1);
        assert(length == sizeof(input) && !memcmp(input, output, length));
        assert(d.rx[index].command & RTL_DESC_OWN);
        assert((d.rx[index].command & RTL_DESC_EOR) ==
               (index == RTL_RX_COUNT - 1 ? RTL_DESC_EOR : 0));
    }
    const uint32_t bad[] = {
        RTL_DESC_FIRST | RTL_DESC_LAST | RTL_RX_ERROR | 64,
        RTL_DESC_FIRST | RTL_DESC_LAST | (1u << 19) | 64,
        RTL_DESC_FIRST | RTL_DESC_LAST | 3,
        RTL_DESC_FIRST | RTL_DESC_LAST | (RTL_BUFFER_SIZE + 1),
        RTL_DESC_FIRST | 64, RTL_DESC_LAST | 64,
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        unsigned index = d.rx_next;
        d.rx[index].command = bad[i];
        assert(rtl_receive(&d, output, sizeof(output), &length) < 0 && !length);
        assert(d.rx[index].command & RTL_DESC_OWN);
    }
    d.rx[d.rx_next].command = RTL_DESC_FIRST | RTL_DESC_LAST | 100;
    assert(rtl_receive(&d, output, 10, &length) == -90 && !length);
    pci_command &= ~4u;
    rtl_free_rings(&d);
    assert(!live);
}

static void lifecycle(uint16_t id, uint32_t xid)
{
    fixture(id, xid);
    int rc = r8169_driver_module.init(&api);
    if (rc)
        fprintf(stderr, "init id=%04x xid=%08x failed %d\n", id, xid, rc);
    assert(rc == 0 && network && live == 4 && mdio_accesses);
    assert(network->is_ready());
    struct reliefos_driver_e1000_info info;
    network->get_info(&info);
    assert(info.present && info.active && info.vendor_id == 0x10ec && info.device_id == id);
    assert(!memcmp(info.mac, "\x02\x11\x22\x33\x44\x55", 6));
    assert(raw32(0x20) && raw32(0x24) && raw32(0xe4) && raw32(0xe8));
    assert((raw32(0x44) & 0xf) == 0xe);
    assert(!(registers[0xe0] & 0x60));
    if (id == 0x8125) {
        assert(!(raw32(0x44) & (1u << 24)) && !(mac_ocp[0xeb58 / 2] & 1));
        assert(phy_ocp[0xa5d4 / 2] & 0x80);
        assert(phy_ocp[0xa408 / 2] == 0x0de1 && phy_ocp[0xa412 / 2] == 0x0300);
    }
    registers[0x6c] = 0;
    assert(!network->is_ready());
    registers[0x6c] = 2;
    assert(network->is_ready());
    reset_stuck = 1;
    network = NULL;
    r8169_driver_module.fini();
    assert(teardown_error < 0 && live == 4 && maps == unmaps + 1);
    reset_stuck = teardown_error = 0;
    finish();
    fixture(id, xid);
    registration_error = -16;
    assert(r8169_driver_module.init(&api) == -16 && !network);
    finish();
    for (unsigned failure = 1; failure <= 4; ++failure) {
        fixture(id, xid);
        allocation_failure = failure;
        assert(r8169_driver_module.init(&api) == -12 && !network);
        finish();
    }
    fixture(id, xid);
    indirect_stuck = 1;
    assert(r8169_driver_module.init(&api) < 0 && !network);
    indirect_stuck = 0;
    finish();
}

int main(void)
{
    assert(r8169_driver_module.kind == RELIEFOS_DRIVER_KIND_NETWORK);
    ring_tests(RTL_FAMILY_8168);
    ring_tests(RTL_FAMILY_8125);
    fixture(0, 0);
    assert(r8169_driver_module.init(&api) == -19);
    finish();
    fixture(0x8168, 0);
    assert(r8169_driver_module.init(&api) == -19);
    finish();
    fixture(0x8125, 0);
    assert(r8169_driver_module.init(&api) == -19);
    finish();
    fixture(0x8168, 0x54100000);
    mapping_failure = 1;
    assert(r8169_driver_module.init(&api) == -12);
    finish();
    const uint32_t gigabit[] = {
        0x30000000, 0x38000000, 0x38400000, 0x3c000000, 0x3c400000,
        0x3c800000, 0x28000000, 0x28800000, 0x2c000000, 0x2c800000,
        0x48000000, 0x4c000000, 0x50800000, 0x54100000, 0x50000000, 0x54800000
    };
    for (unsigned i = 0; i < sizeof(gigabit) / sizeof(gigabit[0]); ++i)
        lifecycle(0x8168, gigabit[i]);
    lifecycle(0x8125, 0x64100000);
    lifecycle(0x8125, 0x60900000);
    lifecycle(0x8125, 0x68800000);
    lifecycle(0x8125, 0x68900000);
    /* A sub-page memory BAR can use the same device's legacy I/O BAR. */
    fixture(0x8168, 0x30000000);
    mapping_failure = 1;
    power_capability = true;
    pmcsr = 0x8003; /* D3hot and a pending PME status, which must survive. */
    struct reliefos_driver_kernel_api port_api = api;
    port_api.inb = io_inb;
    port_api.outb = io_outb;
    port_api.inl = io_inl;
    port_api.outl = io_outl;
    assert(r8169_driver_module.init(&port_api) == 0 && network && maps == unmaps && live == 4);
    assert((pci_command & 7) == 5);
    assert(pmcsr == 0x8000);
    finish();
    puts("r8169: DMA ring wrap, framing, high addresses, failure rollback and lifecycle passed");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Hardware-boundary model for the native Intel I225-V and I226-V driver. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/driver.h>
#include <reliefnt/net_device.h>
#include <reliefnt/lock.h>

extern const struct reliefos_driver_module i225_driver_module __attribute__((weak));

#define CTRL 0x00000u
#define STATUS 0x00008u
#define ICR 0x01500u
#define IMS 0x01508u
#define IMC 0x0150cu
#define IAM 0x01510u
#define EITR 0x01680u
#define EECD 0x00010u
#define EERD 0x12014u
#define MDIC 0x00020u
#define CTRL_EXT 0x00018u
#define PHPM 0x00e14u
#define EEER 0x00e30u
#define IPCNFG 0x00e38u
#define MANC 0x05820u
#define SWSM 0x05b50u
#define SW_FW_SYNC 0x05b5cu
#define RCTL 0x00100u
#define TCTL 0x00400u
#define RXPBS 0x02404u
#define TXPBS 0x03404u
#define RDBAL 0x0c000u
#define RDBAH 0x0c004u
#define RDLEN 0x0c008u
#define RDT 0x0c018u
#define SRRCTL 0x0c00cu
#define RXDCTL 0x0c028u
#define TDBAL 0x0e000u
#define TDBAH 0x0e004u
#define TDLEN 0x0e008u
#define TDT 0x0e018u
#define TXDCTL 0x0e028u
#define RETX_CTL 0x0041cu
#define GTXOFFSET 0x03310u
#define TXQCTL 0x03344u
#define DTXMXPKTSZ 0x0355cu
#define TQAVCTRL 0x03570u
#define RXCSUM 0x05000u
#define RFCTL 0x05008u
#define RLPML 0x05004u
#define MRQC 0x05818u
#define RX_COUNT 256u
#define TX_COUNT 64u
#define RX_DD 1u
#define RX_EOP 2u
#define TX_DD 1u
#define IRQ_LSC (1u << 2)
#define IRQ_RXDMT0 (1u << 4)
#define IRQ_RXT0 (1u << 7)
#define IRQ_MASK (IRQ_LSC | IRQ_RXDMT0 | IRQ_RXT0)

struct rx_desc {
    union {
        struct { uint64_t packet, header; } read;
        struct { uint32_t info, rss, status; uint16_t length, vlan; } wb;
    };
};
struct tx_desc {
    union {
        struct { uint64_t address; uint32_t command, offload; } read;
        struct { uint64_t reserved; uint32_t sequence, status; } wb;
    };
};

struct dma_allocation { uint64_t address; unsigned pages; uint8_t *bytes; };

static uint32_t registers[0x13000 / 4];
#define REG(offset) registers[(offset) / 4]
static uint16_t tested_device_id, device_id, command;
static uint16_t config[128];
static uint8_t pm_capability;
static bool refuse_d0;
static unsigned d0_writes;
static uint16_t phy[32], mmd_address, mmd_aneg;
static unsigned mmd_data_mode;
static uint64_t bar = 0x120000000ULL, physical_next = 0x140000000ULL;
static struct dma_allocation memory[8];
static unsigned live, maps, unmaps, allocation_limit = 4;
static int mapping_failure, registration_error, teardown_error;
static int irq_request_error = -95;
static void (*irq_callback)(void *);
static void *irq_context;
static unsigned irq_requests, irq_frees, notifications;
static bool inject_rx_on_unmask, inject_link_on_unmask;
static unsigned inject_rx_index;
static const struct reliefnt_netdev_ops *network;
static void *network_context;
static uint16_t nvm[64];
static bool nvm_timeout, mdio_timeout, mdio_error, mdio_wrong_register;
static bool phy_reset_timeout;
static bool block_phy_release, block_nvm_release;
static unsigned phy_resets, mdio_commands, nvm_reads, semaphore_rejections;
static unsigned semaphore_requests;
static unsigned queue_delay, pending_rx_enable, pending_tx_enable;
static bool reject_rx_enable, reject_tx_enable;
static uint32_t rx_enable_value, tx_enable_value;
static bool reject_busmaster_disable;
static int enumeration_error;

void *kernel_malloc(size_t size) { return calloc(1, size); }
void kernel_free(void *pointer) { free(pointer); }

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ *flags = 0; assert(!lock->state); lock->state = 1; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }

static uint64_t allocate(unsigned pages, uint64_t mask)
{
    if (live >= allocation_limit)
        return 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].address)
            continue;
        uint8_t *bytes = calloc(pages, 4096);
        assert(bytes);
        uint64_t address = physical_next;
        physical_next += (uint64_t)pages * 4096;
        assert(address + (uint64_t)pages * 4096 - 1 <= mask);
        memory[i] = (struct dma_allocation){address, pages, bytes};
        ++live;
        return address;
    }
    abort();
}

static void release(uint64_t address, unsigned pages)
{
    assert(!(command & 4u));
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].address != address)
            continue;
        assert(memory[i].pages == pages);
        free(memory[i].bytes);
        memory[i] = (struct dma_allocation){0};
        --live;
        return;
    }
    abort();
}

static void *alias(uint64_t address)
{
    for (unsigned i = 0; i < 8; ++i)
        if (memory[i].address && address >= memory[i].address &&
            address - memory[i].address < (uint64_t)memory[i].pages * 4096)
            return memory[i].bytes + (address - memory[i].address);
    return NULL;
}

uint32_t i225_io_read(const volatile uint8_t *base, unsigned offset)
{
    assert(base == (const volatile uint8_t *)registers && offset < sizeof(registers));
    if (offset == RETX_CTL)
        assert(device_id == 0x125c);
    uint32_t value = REG(offset);
    if (offset == ICR)
        REG(ICR) = 0;
    if (offset == SWSM)
        REG(SWSM) |= 1u; /* Read-to-acquire SMBI returns its previous value. */
    return value;
}

void i225_io_write(volatile uint8_t *base, unsigned offset, uint32_t value)
{
    assert(base == (volatile uint8_t *)registers && offset < sizeof(registers));
    if (offset == RETX_CTL)
        assert(device_id == 0x125c && !(command & 4u));
    if (offset == CTRL && (value & (1u << 31)) && !(REG(CTRL) & (1u << 31))) {
        assert(REG(SW_FW_SYNC) & 2u);
        assert(!(REG(MANC) & (1u << 18)));
        REG(PHPM) &= ~(1u << 8);
        ++phy_resets;
    }
    if (offset == CTRL && !(value & (1u << 31)) && (REG(CTRL) & (1u << 31)) &&
        !phy_reset_timeout)
        REG(PHPM) |= 1u << 8;
    if (offset == SWSM && (value & 2u)) {
        assert(REG(SWSM) & 1u);
        ++semaphore_requests;
        if (semaphore_rejections) {
            --semaphore_rejections;
            value &= ~2u;
        }
    }
    if (offset == MDIC) {
        assert(REG(SW_FW_SYNC) & 2u);
        assert(phy_resets && (REG(PHPM) & (1u << 8)));
        ++mdio_commands;
    }
    if ((offset == RXDCTL || offset == TXDCTL) && (value & (1u << 25))) {
        if (offset == TXDCTL && device_id == 0x125c) {
            /* Exercise firmware leaving scheduled TX active across MAC reset. */
            assert(REG(RETX_CTL) == 0xbu && REG(TQAVCTRL) == 0x10u);
            assert(!REG(GTXOFFSET) && !REG(TXQCTL) && REG(DTXMXPKTSZ) == 0x98u);
        }
        if ((offset == RXDCTL && reject_rx_enable) || (offset == TXDCTL && reject_tx_enable))
            value &= ~(1u << 25);
        else if (queue_delay) {
            if (offset == RXDCTL) {
                pending_rx_enable = queue_delay;
                rx_enable_value = value;
            } else {
                pending_tx_enable = queue_delay;
                tx_enable_value = value;
            }
            value &= ~(1u << 25);
        }
    }
    if (offset == RDT && value) {
        assert(REG(RXDCTL) & (1u << 25));
        assert(REG(TXDCTL) & (1u << 25));
        assert(REG(RCTL) & (1u << 1));
    }
    if (offset == IMC) {
        REG(IMS) &= ~value;
        REG(IMC) = value;
        return;
    }
    if (offset == IMS) {
        REG(IMS) |= value;
        if (inject_rx_on_unmask) {
            inject_rx_on_unmask = false;
            uint64_t address = REG(RDBAL) | ((uint64_t)REG(RDBAH) << 32);
            struct rx_desc *ring = alias(address);
            assert(ring && inject_rx_index < RX_COUNT);
            void *buffer = alias(ring[inject_rx_index].read.packet);
            assert(buffer);
            memset(buffer, 0x5a, 64);
            ring[inject_rx_index].wb.info = 0x12345678;
            ring[inject_rx_index].wb.rss = 0x90abcdef;
            ring[inject_rx_index].wb.length = 64;
            ring[inject_rx_index].wb.status = RX_DD | RX_EOP;
            REG(ICR) |= IRQ_RXT0;
        }
        if (inject_link_on_unmask) {
            inject_link_on_unmask = false;
            REG(STATUS) ^= 1u << 1;
            REG(ICR) |= IRQ_LSC;
        }
        return;
    }
    REG(offset) = value;
}

static int enumerate(uint32_t index, struct reliefos_driver_pci_device *out)
{
    if (index)
        return enumeration_error ? enumeration_error : -19;
    *out = (struct reliefos_driver_pci_device){.bus=2,.slot=3,.function=0,
        .class_code=2,.vendor_id=0x8086,.device_id=device_id};
    return 0;
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    assert(bus == 2 && slot == 3 && !function && !(offset & 1));
    return offset == 4 ? command : config[offset / 2];
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value)
{
    assert(bus == 2 && slot == 3 && !function && !(offset & 1));
    if (offset != 4) {
        assert(pm_capability && offset == pm_capability + 4);
        uint16_t old = config[offset / 2];
        config[offset / 2] = (value & ~0x8000u) | (old & ~value & 0x8000u);
        if (refuse_d0)
            config[offset / 2] |= old & 3u;
        assert(!(command & 4u));
        ++d0_writes;
        return;
    }
    if (reject_busmaster_disable)
        value |= command & 4u;
    command = value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    assert(bus == 2 && slot == 3 && !function);
    if (offset == 0x10)
        return (uint32_t)bar | 4u;
    if (offset == 0x14)
        return (uint32_t)(bar >> 32);
    return config[offset / 2] | ((uint32_t)config[offset / 2 + 1] << 16);
}

static void *map_mmio(uint64_t address, uint64_t bytes)
{
    assert(address == bar && bytes == 0x13000 && !(command & 7u));
    if (pm_capability)
        assert(!(config[(pm_capability + 4) / 2] & 3u));
    if (mapping_failure)
        return NULL;
    ++maps;
    return registers;
}

static void unmap_mmio(void *pointer, uint64_t bytes)
{
    assert(pointer == registers && bytes == 0x13000 && !(command & 4u));
    ++unmaps;
}

static void sleep_ms(uint64_t milliseconds)
{
    (void)milliseconds;
    if (pending_rx_enable || pending_tx_enable)
        assert(!(command & 4u));
    if (pending_rx_enable && !--pending_rx_enable)
        REG(RXDCTL) = rx_enable_value;
    if (pending_tx_enable && !--pending_tx_enable)
        REG(TXDCTL) = tx_enable_value;
    if (REG(CTRL) & (1u << 26)) {
        REG(CTRL) &= ~(1u << 26);
        REG(EECD) |= 1u << 9;
    }
    if (REG(EERD) & 1u) {
        unsigned word = (REG(EERD) >> 2) & 0x3fffu;
        assert(word < 64);
        if (!nvm_timeout) {
            REG(EERD) = 2u | (word << 2) | ((uint32_t)nvm[word] << 16);
            ++nvm_reads;
            if (word == 63 && block_nvm_release)
                REG(SWSM) = 1; /* A different software agent acquired SMBI. */
        }
    }
    uint32_t mdic = REG(MDIC);
    unsigned op = (mdic >> 26) & 3u;
    if (op && !(mdic & (1u << 28)) && !mdio_timeout) {
        if (mdio_error) {
            REG(MDIC) = (1u << 28) | (1u << 30);
            return;
        }
        unsigned reg = (mdic >> 16) & 31u;
        uint16_t value = (uint16_t)mdic;
        if (mdic & (1u << 27)) {
            if (reg == 14 && mmd_data_mode)
                value = mmd_aneg;
            else if (reg == 14)
                value = mmd_address;
            else
                value = phy[reg];
        } else if (reg == 13) {
            mmd_data_mode = (value & 0x4000u) != 0;
            phy[reg] = value;
        } else if (reg == 14 && mmd_data_mode) {
            mmd_aneg = value;
        } else if (reg == 14) {
            mmd_address = value;
        } else {
            phy[reg] = value;
            if (reg == 0 && block_phy_release)
                REG(SWSM) = 1;
        }
        REG(MDIC) = (1u << 28) | ((mdio_wrong_register ? reg ^ 1u : reg) << 16) | value;
    }
}

int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context, uint32_t *id)
{
    assert(command & 4u);
    assert((REG(RXDCTL) & (1u << 25)) && (REG(TXDCTL) & (1u << 25)));
    assert(REG(CTRL_EXT) & (1u << 28));
    if (registration_error)
        return registration_error;
    network = ops;
    network_context = context;
    *id = 3;
    return 0;
}

static void report_teardown_failure(int error) { teardown_error = error; }
static void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static int request_pci_irq(const struct reliefos_driver_pci_device *device,
                           void (*handler)(void *), void *context, uint32_t *handle)
{
    assert(device->vendor_id == 0x8086 && device->device_id == tested_device_id);
    ++irq_requests;
    if (irq_request_error)
        return irq_request_error;
    irq_callback = handler;
    irq_context = context;
    *handle = 17;
    return 0;
}

static void free_pci_irq(uint32_t handle)
{
    assert(handle == 17 && irq_callback && irq_context);
    unsigned before = notifications;
    irq_callback(irq_context); /* A message already in flight drains before free returns. */
    assert(notifications == before && !(REG(IMS) & IRQ_MASK));
    irq_callback = NULL;
    irq_context = NULL;
    ++irq_frees;
}

void netdev_rx_notify(uint32_t ifindex)
{
    assert(ifindex == 3);
    ++notifications;
}

static const struct reliefos_driver_kernel_api api = {
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION,.struct_size=sizeof(api),
    .pci_enumerate=enumerate,.pci_read16=pci_read16,.pci_write16=pci_write16,
    .pci_read32=pci_read32,.map_mmio=map_mmio,.unmap_mmio=unmap_mmio,
    .phys_to_virt=alias,.alloc_dma=allocate,.free_dma=release,.sleep_ms=sleep_ms,
    .request_pci_irq=request_pci_irq,.free_pci_irq=free_pci_irq,
    .report_teardown_failure=report_teardown_failure,.console_log=console_log,
};

static void reset_fixture(void)
{
    assert(!live && maps == unmaps);
    assert(!irq_callback && !irq_context);
    memset(registers, 0, sizeof(registers));
    memset(phy, 0, sizeof(phy));
    memset(nvm, 0, sizeof(nvm));
    memset(config, 0, sizeof(config));
    pm_capability = 0;
    refuse_d0 = false;
    d0_writes = 0;
    nvm[0] = 0x5452;
    nvm[1] = 0x1200;
    nvm[2] = 0x5634;
    nvm[63] = (uint16_t)(0xbaba - nvm[0] - nvm[1] - nvm[2]);
    nvm_timeout = mdio_timeout = mdio_error = mdio_wrong_register = false;
    phy_reset_timeout = false;
    block_phy_release = block_nvm_release = false;
    phy_resets = mdio_commands = semaphore_rejections = semaphore_requests = 0;
    queue_delay = pending_rx_enable = pending_tx_enable = 0;
    reject_rx_enable = reject_tx_enable = false;
    nvm_reads = 0;
    REG(STATUS) = 1u << 1;
    REG(EECD) = 1u << 9;
    REG(IAM) = UINT32_MAX;
    REG(RXCSUM) = REG(MRQC) = UINT32_MAX;
    REG(CTRL) = (1u << 27) | (1u << 28) | (1u << 30);
    REG(CTRL_EXT) = 0x55aa;
    REG(EEER) = (7u << 16) | 0x125;
    REG(IPCNFG) = 0x11c;
    REG(RXPBS) = REG(TXPBS) = REG(RFCTL) = UINT32_MAX;
    if (tested_device_id == 0x125c) {
        REG(RETX_CTL) = 0x1b0bu;
        REG(GTXOFFSET) = 0x578u;
        REG(TXQCTL) = 0xc7u;
        REG(DTXMXPKTSZ) = 0x19u;
        REG(TQAVCTRL) = 0x99u;
    }
    for (unsigned i = 1; i < 16; ++i) {
        REG(0x5400 + i * 8) = 0xa5a5a5a5;
        REG(0x5404 + i * 8) = 0x80001234;
    }
    REG(0x5400) = 0x12005452;
    REG(0x5404) = (1u << 31) | 0x5634;
    phy[2] = 0x0141;
    phy[3] = 0x0dd1;
    phy[4] = 0x0de1;
    phy[9] = 0x0300;
    phy[0] = (1u << 14) | (1u << 11) | (1u << 10);
    mmd_address = mmd_aneg = 0;
    mmd_data_mode = 0;
    device_id = tested_device_id;
    command = 7;
    allocation_limit = 4;
    mapping_failure = registration_error = teardown_error = 0;
    irq_request_error = -95;
    irq_requests = irq_frees = notifications = 0;
    inject_rx_on_unmask = inject_link_on_unmask = false;
    inject_rx_index = 0;
    reject_busmaster_disable = false;
    enumeration_error = 0;
    network = NULL;
    network_context = NULL;
}

static void finish(void)
{
    i225_driver_module.fini();
    assert(!teardown_error && !live && maps == unmaps && command == 7);
    assert(!(REG(CTRL_EXT) & (1u << 28)));
    assert(!irq_callback && !irq_context);
    network = NULL;
    network_context = NULL;
}

static struct rx_desc *rx_ring(void)
{
    uint64_t address = REG(RDBAL) | ((uint64_t)REG(RDBAH) << 32);
    assert(address > 0xffffffffULL);
    struct rx_desc *result = alias(address);
    assert(result && (void *)result != (void *)(uintptr_t)address);
    return result;
}

static struct tx_desc *tx_ring(void)
{
    uint64_t address = REG(TDBAL) | ((uint64_t)REG(TDBAH) << 32);
    assert(address > 0xffffffffULL);
    struct tx_desc *result = alias(address);
    assert(result && (void *)result != (void *)(uintptr_t)address);
    return result;
}

static void test_power_management(void)
{
    /* Traverse an unrelated MSI capability before PM; preserve pending PME. */
    reset_fixture();
    config[6 / 2] = 1u << 4;
    config[0x34 / 2] = 0x40;
    config[0x40 / 2] = 0x6005;
    config[0x60 / 2] = 1;
    pm_capability = 0x60;
    config[0x64 / 2] = 0x8103;
    assert(!i225_driver_module.init(&api));
    assert(d0_writes == 1 && config[0x64 / 2] == 0x8100);
    assert(network && network->is_ready(network_context));
    finish();

    reset_fixture();
    config[6 / 2] = 1u << 4;
    config[0x34 / 2] = 0x40;
    config[0x40 / 2] = 5; /* No PM capability is also a valid conventional chain. */
    assert(!i225_driver_module.init(&api));
    assert(!d0_writes);
    finish();

    const uint8_t corrupt[] = {0x3c, 0x42, 0xff};
    for (unsigned i = 0; i < sizeof(corrupt); ++i) {
        reset_fixture();
        config[6 / 2] = 1u << 4;
        config[0x34 / 2] = corrupt[i];
        unsigned previous_maps = maps;
        assert(i225_driver_module.init(&api) == -22);
        assert(maps == previous_maps && !network && !live);
        finish();
    }

    reset_fixture();
    config[6 / 2] = 1u << 4;
    config[0x34 / 2] = 0x40;
    config[0x40 / 2] = 0x6005;
    config[0x60 / 2] = 0x4005; /* A cycle must terminate without MMIO. */
    unsigned previous_maps = maps;
    assert(i225_driver_module.init(&api) == -22);
    assert(maps == previous_maps && !network && !live);
    finish();

    reset_fixture();
    config[6 / 2] = 1u << 4;
    config[0x34 / 2] = 0xfc;
    config[0xfc / 2] = 1; /* PMCSR would extend past conventional config space. */
    previous_maps = maps;
    assert(i225_driver_module.init(&api) == -22);
    assert(maps == previous_maps && !network);
    finish();

    reset_fixture();
    config[6 / 2] = 1u << 4;
    config[0x34 / 2] = pm_capability = 0x50;
    config[0x50 / 2] = 1;
    config[0x54 / 2] = 0x8103;
    refuse_d0 = true;
    previous_maps = maps;
    assert(i225_driver_module.init(&api) == -5);
    assert(d0_writes == 1 && config[0x54 / 2] == 0x8103);
    assert(maps == previous_maps && !network && !live);
    finish();
}

static void test_phy_and_semaphores(void)
{
    reset_fixture();
    REG(MANC) = 1u << 18;
    assert(i225_driver_module.init(&api) == -16);
    assert(!phy_resets && !mdio_commands && !network && !live);
    assert(REG(EEER) == ((7u << 16) | 0x125) && REG(IPCNFG) == 0x11c);
    finish();

    reset_fixture();
    phy_reset_timeout = true;
    assert(i225_driver_module.init(&api) == -110);
    assert(phy_resets == 1 && !mdio_commands && !network && !live);
    assert(!(REG(CTRL) & (1u << 31)) && !REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    mdio_wrong_register = true;
    assert(i225_driver_module.init(&api) == -5);
    assert(mdio_commands == 1 && !network && !live);
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    semaphore_rejections = 3;
    assert(!i225_driver_module.init(&api));
    assert(semaphore_requests >= 4 && !semaphore_rejections);
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    assert(phy_resets == 1 && REG(EEER) == 0x125 && REG(IPCNFG) == 0x100);
    assert(REG(CTRL_EXT) == ((1u << 28) | 0x55aa));
    finish();
    assert(REG(CTRL_EXT) == 0x55aa);

    reset_fixture();
    semaphore_rejections = 100;
    assert(i225_driver_module.init(&api) == -16);
    assert(semaphore_requests == 100 && !REG(SWSM) && !REG(SW_FW_SYNC));
    assert(!phy_resets && !network && !live);
    finish();

    reset_fixture();
    REG(SWSM) = 1; /* Another software owner must not have its lock cleared. */
    assert(i225_driver_module.init(&api) == -16);
    assert(REG(SWSM) == 1 && !semaphore_requests && !phy_resets && !network);
    finish();

    reset_fixture();
    REG(SW_FW_SYNC) = 2u << 16; /* Firmware owns the PHY resource. */
    assert(i225_driver_module.init(&api) == -16);
    assert(REG(SW_FW_SYNC) == (2u << 16) && !REG(SWSM));
    assert(!phy_resets && !mdio_commands && !network);
    finish();

    /* Ownership survives release failure until STOP can obtain SWSM again. */
    for (unsigned use_nvm = 0; use_nvm < 2; ++use_nvm) {
        reset_fixture();
        uint32_t unrelated = (1u << 4) | (1u << 20);
        REG(SW_FW_SYNC) = unrelated;
        if (use_nvm) {
            REG(0x5404) = 0;
            block_nvm_release = true;
        } else {
            block_phy_release = true;
        }
        uint32_t owned = use_nvm ? 1 : 2;
        unsigned previous_unmaps = unmaps;
        assert(i225_driver_module.init(&api) == -16);
        assert(!network && !live && maps == previous_unmaps + 1 && unmaps == previous_unmaps);
        assert(REG(SW_FW_SYNC) == (unrelated | owned) && REG(SWSM) == 1);
        assert(REG(CTRL_EXT) & (1u << 28));
        i225_driver_module.fini();
        assert(teardown_error == -16 && unmaps == previous_unmaps);
        assert(REG(SW_FW_SYNC) == (unrelated | owned) && REG(SWSM) == 1);
        REG(SWSM) = 0; /* External owner releases; normal unload may now finish. */
        teardown_error = 0;
        finish();
        assert(REG(SW_FW_SYNC) == unrelated && !REG(SWSM));
    }
}

static void test_queue_startup(void)
{
    reset_fixture();
    queue_delay = 3;
    assert(!i225_driver_module.init(&api));
    assert(!pending_rx_enable && !pending_tx_enable && network);
    assert(REG(RDT) == RX_COUNT - 1);
    assert(REG(RXPBS) == 0xa2 && REG(TXPBS) == 0x04000014);
    assert(REG(RFCTL) == (1u << 16) && (REG(RCTL) & (1u << 22)));
    for (unsigned i = 1; i < 15; ++i)
        assert(!REG(0x5400 + i * 8) && !REG(0x5404 + i * 8));
    assert(REG(0x5478) == 0xa5a5a5a5 && REG(0x547c) == 0x80001234);
    finish();

    for (unsigned i = 0; i < 2; ++i) {
        reset_fixture();
        irq_request_error = 0;
        reject_rx_enable = i == 0;
        reject_tx_enable = i == 1;
        assert(i225_driver_module.init(&api) == -110);
        assert(!network && !live && irq_frees == 1 && !irq_callback && !REG(RDT));
        finish();
    }
}

static void test_frame_boundaries(void)
{
    reset_fixture();
    assert(!i225_driver_module.init(&api));
    struct tx_desc *tx = tx_ring();
    uint8_t long_frame[1515];
    memset(long_frame, 0xa5, sizeof(long_frame));
    /* Fill every slot once with a long packet so padding must erase reused data. */
    for (unsigned i = 0; i < TX_COUNT; ++i) {
        assert(!network->send(network_context, long_frame, 1514));
        tx[i].wb.status = TX_DD;
    }
    for (unsigned length = 14; length <= 61; ++length) {
        uint8_t *frame = malloc(length);
        assert(frame);
        memset(frame, 0x5a, length);
        unsigned index = REG(TDT);
        assert(!network->send(network_context, frame, length));
        free(frame);
        unsigned wire_length = length < 60 ? 60 : length;
        assert((tx[index].read.command & 0xffffu) == wire_length);
        assert(tx[index].read.offload == wire_length << 14);
        uint8_t *bytes = alias(tx[index].read.address);
        for (unsigned i = 0; i < wire_length; ++i)
            assert(bytes[i] == (i < length ? 0x5a : 0));
        assert(bytes[wire_length] == 0xa5);
        tx[index].wb.status = TX_DD;
    }
    unsigned tail = REG(TDT);
    assert(network->send(network_context, long_frame, 1515) == -90 && REG(TDT) == tail);
    struct rx_desc *rx = rx_ring();
    uint8_t output[1518];
    memcpy(alias(rx[0].read.packet), long_frame, 1514);
    rx[0].wb.length = 1514;
    rx[0].wb.status = RX_DD | RX_EOP;
    uint32_t length = 0;
    assert(REG(RLPML) == 1518);
    assert(network->poll(network_context, output, sizeof(output), &length) == 1);
    assert(length == 1514 && !memcmp(output, long_frame, length));
    rx[1].wb.length = 1515;
    rx[1].wb.status = RX_DD | RX_EOP;
    assert(network->poll(network_context, output, sizeof(output), &length) == -90 && !length);
    finish();
}

static void test_device(uint16_t id)
{
    tested_device_id = id;
    test_power_management();
    test_phy_and_semaphores();
    test_queue_startup();
    test_frame_boundaries();

    for (unsigned failure = 0; failure < 4; ++failure) {
        reset_fixture();
        allocation_limit = failure;
        assert(i225_driver_module.init(&api) == -12);
        finish();
    }

    reset_fixture();
    mapping_failure = 1;
    assert(i225_driver_module.init(&api) == -12);
    finish();

    reset_fixture();
    registration_error = -16;
    assert(i225_driver_module.init(&api) == -16);
    finish();

    reset_fixture();
    REG(0x5404) = 0; /* Read a valid address from shadow NVM under the firmware lock. */
    REG(0x5400) = 0;
    assert(!i225_driver_module.init(&api));
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    assert(nvm_reads == 64);
    assert(!memcmp(network->mac(network_context), "\x52\x54\x00\x12\x34\x56", 6));
    finish();

    reset_fixture();
    REG(0x5404) = 0;
    nvm[63] ^= 1;
    assert(i225_driver_module.init(&api) == -5);
    assert(nvm_reads == 64 && !REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    REG(0x5404) = 0;
    nvm_timeout = true;
    assert(i225_driver_module.init(&api) == -110);
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    mdio_error = true;
    assert(i225_driver_module.init(&api) == -5);
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    mdio_timeout = true;
    assert(i225_driver_module.init(&api) == -110);
    assert(!REG(SW_FW_SYNC) && !REG(SWSM));
    finish();

    reset_fixture();
    REG(STATUS) = 0; /* No cable does not prevent the interface from registering. */
    assert(!i225_driver_module.init(&api));
    assert(network && !network->is_ready(network_context));
    finish();

    reset_fixture();
    assert(!i225_driver_module.init(&api));
    assert(irq_requests == 1 && !irq_callback && !(REG(IMS) & IRQ_MASK));
    assert(network && network->is_ready(network_context));
    assert(!REG(IAM));
    assert(REG(EITR) == (196u | (1u << 31)));
    assert(!REG(RXCSUM) && !REG(MRQC) && REG(RLPML) == 1518);
    assert(!(REG(CTRL) & ((1u << 27) | (1u << 28) | (1u << 30))));
    assert(REG(RDLEN) == RX_COUNT * 16 && REG(TDLEN) == TX_COUNT * 16);
    assert((REG(SRRCTL) & (3u << 25)) == (1u << 25));
    assert((REG(RXDCTL) & (1u << 25)) && (REG(TXDCTL) & (1u << 25)));
    assert((REG(RCTL) & ((1u << 1) | (1u << 15) | (1u << 26))) ==
           ((1u << 1) | (1u << 15) | (1u << 26)));
    assert((phy[4] & 0x0fe0u) == 0x0140u && (phy[9] & 0x0300u) == 0x0200u);
    assert((mmd_aneg & 0x0080u) && (phy[0] & ((1u << 12) | (1u << 9))) ==
           ((1u << 12) | (1u << 9)));
    assert(!(phy[0] & ((1u << 14) | (1u << 11) | (1u << 10))));

    struct e1000_info info = {0};
    network->get_info(network_context, &info);
    assert(info.present && info.active && info.device_id == id && info.bus == 2);
    assert(!memcmp(network->mac(network_context), "\x52\x54\x00\x12\x34\x56", 6));

    struct rx_desc *rx = rx_ring();
    struct tx_desc *tx = tx_ring();
    unsigned char frame[1500], output[2048];
    for (unsigned i = 0; i < sizeof(frame); ++i)
        frame[i] = (unsigned char)(i * 31 + 7);
    assert(network->send(network_context, NULL, sizeof(frame)) == -22);
    assert(network->send(network_context, frame, 13) == -22);
    assert(network->send(network_context, frame, 1519) == -90);
    assert(!network->send(network_context, frame, sizeof(frame)));
    assert(REG(TDT) == 1 && tx[0].read.address > 0xffffffffULL);
    assert((tx[0].read.command & (0x00300000u | (1u << 29) | (1u << 25) |
                                  (1u << 24) | (1u << 27))) ==
           (0x00300000u | (1u << 29) | (1u << 25) | (1u << 24) | (1u << 27)));
    assert((tx[0].read.command & 0xffffu) == sizeof(frame));
    assert(!memcmp(alias(tx[0].read.address), frame, sizeof(frame)));
    assert(!(tx[0].wb.status & TX_DD)); /* olinfo_status shares the writeback DD dword. */
    tx[0].wb.status = TX_DD;
    for (unsigned i = 0; i < TX_COUNT - 1; ++i)
        assert(!network->send(network_context, frame, sizeof(frame)));
    assert(network->send(network_context, frame, sizeof(frame)) == -11);
    for (unsigned i = 0; i < TX_COUNT; ++i)
        tx[i].wb.status = TX_DD;
    for (unsigned n = 0; n < TX_COUNT * 3 + 1; ++n) {
        unsigned next = REG(TDT);
        assert(!network->send(network_context, frame, sizeof(frame)));
        assert(!(tx[next].wb.status & TX_DD));
        assert(!memcmp(alias(tx[next].read.address), frame, sizeof(frame)));
        tx[next].wb.status = TX_DD;
    }

    for (unsigned n = 0; n < RX_COUNT * 2 + 3; ++n) {
        unsigned index = n % RX_COUNT;
        uint64_t packet = rx[index].read.packet;
        assert(packet > 0xffffffffULL);
        void *buffer = alias(packet);
        assert(buffer);
        memcpy(buffer, frame, sizeof(frame));
        rx[index].wb.info = 0x11223344;
        rx[index].wb.rss = 0x55667788;
        rx[index].wb.length = sizeof(frame);
        rx[index].wb.vlan = 0xa55a;
        rx[index].wb.status = RX_DD | RX_EOP;
        uint32_t length = 0;
        assert(network->poll(network_context, output, sizeof(output), &length) == 1);
        assert(length == sizeof(frame) && !memcmp(output, frame, sizeof(frame)));
        assert(rx[index].read.packet == packet && !rx[index].read.header && REG(RDT) == index);
    }
    unsigned index = (RX_COUNT * 2 + 3) % RX_COUNT;
    rx[index].wb.length = 64;
    rx[index].wb.status = RX_DD | RX_EOP | (1u << 31);
    assert(network->poll(network_context, output, sizeof(output), NULL) == -74);
    for (unsigned fragment = 0; fragment < 3; ++fragment) {
        index = (index + 1) % RX_COUNT;
        rx[index].wb.length = 64;
        rx[index].wb.status = RX_DD | (fragment == 2 ? RX_EOP : 0);
        uint32_t dropped = 99;
        assert(network->poll(network_context, output, sizeof(output), &dropped) == -74);
        assert(!dropped && rx[index].read.packet > 0xffffffffULL && !rx[index].read.header);
    }
    index = (index + 1) % RX_COUNT;
    memset(alias(rx[index].read.packet), 0x7e, 64);
    rx[index].wb.length = 64;
    rx[index].wb.status = RX_DD | RX_EOP;
    uint32_t recovered_length = 0;
    assert(network->poll(network_context, output, sizeof(output), &recovered_length) == 1);
    assert(recovered_length == 64 && output[0] == 0x7e && output[63] == 0x7e);
    index = (index + 1) % RX_COUNT;
    rx[index].wb.length = 1519;
    rx[index].wb.status = RX_DD | RX_EOP;
    assert(network->poll(network_context, output, sizeof(output), &recovered_length) == -90);
    assert(!recovered_length && !rx[index].read.header);
    index = (index + 1) % RX_COUNT;
    rx[index].wb.length = 64;
    rx[index].wb.status = RX_DD | RX_EOP;
    assert(network->poll(network_context, output, 63, &recovered_length) == -90);
    assert(!recovered_length && !rx[index].read.header);
    REG(STATUS) = 0;
    assert(!network->is_ready(network_context));
    assert(network->send(network_context, frame, sizeof(frame)) == -100);
    REG(STATUS) = 1u << 1;
    assert(network->is_ready(network_context));
    finish();

    reset_fixture();
    irq_request_error = 0;
    assert(!i225_driver_module.init(&api));
    assert(irq_callback && irq_context && irq_requests == 1 && !irq_frees);
    assert((REG(IMS) & IRQ_MASK) == IRQ_MASK);
    assert(notifications == 1); /* Initial task pass drains frames received before publication. */
    REG(ICR) = IRQ_RXT0;
    irq_callback(irq_context);
    assert(!(REG(IMS) & IRQ_MASK) && !REG(ICR) && notifications == 2);
    assert(network->poll(network_context, output, sizeof(output), NULL) == 0);
    assert((REG(IMS) & IRQ_MASK) == IRQ_MASK);

    rx = rx_ring();
    uint64_t packet = rx[0].read.packet;
    memcpy(alias(packet), frame, sizeof(frame));
    rx[0].wb.info = 0x11223344;
    rx[0].wb.rss = 0x55667788;
    rx[0].wb.length = sizeof(frame);
    rx[0].wb.status = RX_DD | RX_EOP;
    REG(ICR) = IRQ_RXT0;
    irq_callback(irq_context);
    uint32_t irq_length = 0;
    assert(network->poll(network_context, output, sizeof(output), &irq_length) == 1);
    assert(irq_length == sizeof(frame) && !memcmp(output, frame, sizeof(frame)));
    assert(network->poll(network_context, output, sizeof(output), NULL) == 0);
    assert((REG(IMS) & IRQ_MASK) == IRQ_MASK && rx[0].read.packet == packet);

    irq_callback(irq_context); /* A stale MSI with no current cause is harmless. */
    unsigned before = notifications;
    inject_rx_index = 1;
    inject_rx_on_unmask = true;
    assert(network->poll(network_context, output, sizeof(output), NULL) == 0);
    assert(notifications == before + 1 && !(REG(IMS) & IRQ_MASK));
    assert(network->poll(network_context, output, sizeof(output), &irq_length) == 1);
    assert(irq_length == 64 && output[0] == 0x5a && output[63] == 0x5a);
    assert(network->poll(network_context, output, sizeof(output), NULL) == 0);
    assert((REG(IMS) & IRQ_MASK) == IRQ_MASK);

    REG(STATUS) = 0;
    REG(ICR) = IRQ_LSC;
    irq_callback(irq_context);
    assert(!network->is_ready(network_context));
    before = notifications;
    inject_link_on_unmask = true;
    assert(network->poll(network_context, output, sizeof(output), NULL) == 0);
    assert(network->is_ready(network_context) && notifications == before + 1);
    assert((REG(IMS) & IRQ_MASK) == IRQ_MASK && !REG(ICR));
    finish();
    assert(irq_requests == 1 && irq_frees == 1);

    reset_fixture();
    irq_request_error = 0;
    registration_error = -16;
    assert(i225_driver_module.init(&api) == -16);
    assert(!irq_callback && !irq_context && irq_requests == 1 && irq_frees == 1);
    finish();

    reset_fixture();
    irq_request_error = 0;
    struct reliefos_driver_kernel_api no_irq_api = api;
    no_irq_api.request_pci_irq = NULL;
    no_irq_api.free_pci_irq = NULL;
    assert(!i225_driver_module.init(&no_irq_api));
    assert(!irq_requests && !irq_callback);
    finish();

    reset_fixture();
    irq_request_error = 0;
    assert(!i225_driver_module.init(&api));
    REG(STATUS) |= 1u << 19; /* DMA master remains busy; STOP must retain every lease. */
    unsigned maps_before = maps, unmaps_before = unmaps;
    i225_driver_module.fini();
    assert(teardown_error == -110 && live == 4 && maps == maps_before && unmaps == unmaps_before);
    assert(!irq_callback && irq_frees == 1 && !network->is_ready(network_context));
    REG(STATUS) &= ~(1u << 19);
    teardown_error = 0;
    finish(); /* A later unload retries STOP and releases the retained buffers. */

    reset_fixture();
    assert(!i225_driver_module.init(&api));
    reject_busmaster_disable = true;
    unmaps_before = unmaps;
    i225_driver_module.fini();
    assert(teardown_error == -5 && live == 4 && unmaps == unmaps_before);
    reject_busmaster_disable = false;
    teardown_error = 0;
    finish();

    reset_fixture();
    enumeration_error = -5;
    assert(!i225_driver_module.init(&api)); /* A later PCI scan failure keeps a working NIC. */
    assert(network && network->is_ready(network_context));
    finish();

    printf("PASS i225 8086:%04x: PCI D0, PHY, TX mode, RX/TX, MSI and DMA STOP\n", id);
}

int main(void)
{
    if (!&i225_driver_module) {
        fputs("FAIL i225: native Intel 2.5GbE driver is missing\n", stderr);
        return 1;
    }
    assert(i225_driver_module.init && i225_driver_module.fini);
    assert(!strcmp(i225_driver_module.name, "i225"));
    assert(i225_driver_module.init(NULL) == -22);
    struct reliefos_driver_kernel_api truncated_api = api;
    truncated_api.struct_size = RELIEFOS_DRIVER_PHYS_MAP_API_SIZE - 1;
    assert(i225_driver_module.init(&truncated_api) == -22);

    test_device(0x15f3);
    test_device(0x125c);

    /* Other SKUs and blank-NVM devices must never be probed. */
    const uint16_t unsupported[] = {0x15f2, 0x125b, 0x125d, 0x125f};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        reset_fixture();
        device_id = unsupported[i];
        unsigned previous_maps = maps;
        assert(i225_driver_module.init(&api) == -19);
        assert(maps == previous_maps && !irq_requests && !network);
        finish();
    }
    return 0;
}

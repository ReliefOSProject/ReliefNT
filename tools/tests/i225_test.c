/* SPDX-License-Identifier: Apache-2.0 */
/* Hardware-boundary model for the native Intel I225-V driver. */
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
#define EECD 0x00010u
#define EERD 0x12014u
#define MDIC 0x00020u
#define SWSM 0x05b50u
#define SW_FW_SYNC 0x05b5cu
#define RCTL 0x00100u
#define TCTL 0x00400u
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
#define RX_COUNT 256u
#define TX_COUNT 64u
#define RX_DD 1u
#define RX_EOP 2u
#define TX_DD 1u

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
static uint16_t device_id = 0x15f3, command = 7;
static uint16_t phy[32], mmd_address, mmd_aneg;
static unsigned mmd_data_mode;
static uint64_t bar = 0x120000000ULL, physical_next = 0x140000000ULL;
static struct dma_allocation memory[8];
static unsigned live, maps, unmaps, allocation_limit = 4;
static int mapping_failure, registration_error, teardown_error;
static const struct reliefnt_netdev_ops *network;
static void *network_context;
static const uint16_t nvm_mac[3] = {0x5452, 0x1200, 0x5634};

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

static int enumerate(uint32_t index, struct reliefos_driver_pci_device *out)
{
    if (index)
        return -19;
    *out = (struct reliefos_driver_pci_device){.bus=2,.slot=3,.function=0,
        .class_code=2,.vendor_id=0x8086,.device_id=device_id};
    return 0;
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{ assert(bus == 2 && slot == 3 && !function && offset == 4); return command; }

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value)
{ assert(bus == 2 && slot == 3 && !function && offset == 4); command = value; }

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    assert(bus == 2 && slot == 3 && !function);
    if (offset == 0x10)
        return (uint32_t)bar | 4u;
    if (offset == 0x14)
        return (uint32_t)(bar >> 32);
    return 0;
}

static void *map_mmio(uint64_t address, uint64_t bytes)
{
    assert(address == bar && bytes == 0x13000 && !(command & 7u));
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
    if (REG(CTRL) & (1u << 26)) {
        REG(CTRL) &= ~(1u << 26);
        REG(EECD) |= 1u << 9;
    }
    if (REG(EERD) & 1u) {
        unsigned word = (REG(EERD) >> 2) & 0x3fffu;
        assert(word < 3);
        REG(EERD) = 2u | (word << 2) | ((uint32_t)nvm_mac[word] << 16);
    }
    uint32_t mdic = REG(MDIC);
    unsigned op = (mdic >> 26) & 3u;
    if (op && !(mdic & (1u << 28))) {
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
        }
        REG(MDIC) = (1u << 28) | value;
    }
}

int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context, uint32_t *id)
{
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

static const struct reliefos_driver_kernel_api api = {
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION,.struct_size=sizeof(api),
    .pci_enumerate=enumerate,.pci_read16=pci_read16,.pci_write16=pci_write16,
    .pci_read32=pci_read32,.map_mmio=map_mmio,.unmap_mmio=unmap_mmio,
    .phys_to_virt=alias,.alloc_dma=allocate,.free_dma=release,.sleep_ms=sleep_ms,
    .report_teardown_failure=report_teardown_failure,.console_log=console_log,
};

static void reset_fixture(void)
{
    assert(!live && maps == unmaps);
    memset(registers, 0, sizeof(registers));
    memset(phy, 0, sizeof(phy));
    REG(STATUS) = 1u << 1;
    REG(EECD) = 1u << 9;
    REG(0x5400) = 0x12005452;
    REG(0x5404) = (1u << 31) | 0x5634;
    phy[2] = 0x0141;
    phy[3] = 0x0dd1;
    phy[4] = 0x0de1;
    phy[9] = 0x0300;
    mmd_address = mmd_aneg = 0;
    mmd_data_mode = 0;
    device_id = 0x15f3;
    command = 7;
    allocation_limit = 4;
    mapping_failure = registration_error = teardown_error = 0;
    network = NULL;
    network_context = NULL;
}

static void finish(void)
{
    i225_driver_module.fini();
    assert(!teardown_error && !live && maps == unmaps && command == 7);
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

int main(void)
{
    if (!&i225_driver_module) {
        fputs("FAIL i225: native I225-V driver is missing\n", stderr);
        return 1;
    }
    assert(i225_driver_module.init && i225_driver_module.fini);
    assert(!strcmp(i225_driver_module.name, "i225"));
    assert(i225_driver_module.init(NULL) == -22);

    reset_fixture();
    device_id = 0x125c; /* I226-V is deliberately outside the first device table. */
    assert(i225_driver_module.init(&api) == -19);
    finish();

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
    assert(!memcmp(network->mac(network_context), "\x52\x54\x00\x12\x34\x56", 6));
    finish();

    reset_fixture();
    REG(STATUS) = 0; /* No cable does not prevent the interface from registering. */
    assert(!i225_driver_module.init(&api));
    assert(network && !network->is_ready(network_context));
    finish();

    reset_fixture();
    assert(!i225_driver_module.init(&api));
    assert(network && network->is_ready(network_context));
    assert(REG(RDLEN) == RX_COUNT * 16 && REG(TDLEN) == TX_COUNT * 16);
    assert((REG(SRRCTL) & (3u << 25)) == (1u << 25));
    assert((REG(RXDCTL) & (1u << 25)) && (REG(TXDCTL) & (1u << 25)));
    assert((REG(RCTL) & ((1u << 1) | (1u << 15) | (1u << 26))) ==
           ((1u << 1) | (1u << 15) | (1u << 26)));
    assert((phy[4] & 0x0fe0u) == 0x0140u && (phy[9] & 0x0300u) == 0x0200u);
    assert((mmd_aneg & 0x0080u) && (phy[0] & ((1u << 12) | (1u << 9))) ==
           ((1u << 12) | (1u << 9)));

    struct e1000_info info = {0};
    network->get_info(network_context, &info);
    assert(info.present && info.active && info.device_id == 0x15f3 && info.bus == 2);
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

    for (unsigned n = 0; n < RX_COUNT * 2 + 3; ++n) {
        unsigned index = n % RX_COUNT;
        memcpy(alias(rx[index].read.packet), frame, sizeof(frame));
        rx[index].wb.length = sizeof(frame);
        rx[index].wb.status = RX_DD | RX_EOP;
        uint32_t length = 0;
        assert(network->poll(network_context, output, sizeof(output), &length) == 1);
        assert(length == sizeof(frame) && !memcmp(output, frame, sizeof(frame)));
        assert(!rx[index].wb.status && REG(RDT) == index);
    }
    unsigned index = (RX_COUNT * 2 + 3) % RX_COUNT;
    rx[index].wb.length = 64;
    rx[index].wb.status = RX_DD | RX_EOP | (1u << 31);
    assert(network->poll(network_context, output, sizeof(output), NULL) == -74);
    finish();

    puts("PASS i225: exact PCI match, high BAR/DMA, advanced queues, TX/RX wrap, errors and cleanup");
    return 0;
}

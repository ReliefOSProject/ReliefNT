/* SPDX-License-Identifier: Apache-2.0 */
/* Production driver objects; only actual PCI, ports, DMA and core services
 * are replaced. The model wraps RX bytes at the configured ring boundary. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/net/rtl8139/internal.h"
#include <reliefnt/net_device.h>
#include <reliefnt/time.h>

extern const struct reliefos_driver_module rtl8139_driver_module;

struct model {
    uint8_t reg[256], config[256];
    uint16_t command, port;
    uint32_t bar, producer;
    unsigned resets, kicks, abort_clears;
    bool reset_stuck, disable_stuck, lost, pm_cap;
    struct reliefos_driver_pci_device pci;
};
static struct model models[2];
static unsigned model_count, registered, live, heap_live, attempts, fail_allocation, fail_alias;
static unsigned sleeps, enumerated;
static uint64_t now, next_physical;
static int registration_error, enumeration_error, teardown_error;
static bool high_dma;
static const struct reliefnt_netdev_ops *networks[2];
static struct r8139_device *contexts[2];
static struct model *initializing;
static struct allocation {
    uint64_t physical;
    unsigned pages;
    void *bytes;
    struct model *owner;
} memory[8];

/** @brief Read an unaligned little-endian model register. */
static uint32_t word(const uint8_t *p, unsigned size)
{
    uint32_t value = 0;
    memcpy(&value, p, size);
    return value;
}

/** @brief Store an unaligned little-endian model register. */
static void store(uint8_t *p, uint32_t value, unsigned size)
{
    memcpy(p, &value, size);
}

/** @brief Select exactly the modeled PCI function addressed by a config access. */
static struct model *pci_model(uint8_t bus, uint8_t slot, uint8_t function)
{
    for (unsigned i = 0; i < model_count; ++i)
        if (models[i].pci.bus == bus && models[i].pci.slot == slot &&
            models[i].pci.function == function)
            return &models[i];
    abort();
}

/** @brief Select the exact port window, rejecting wrong I/O argument order. */
static struct model *port_model(uint16_t port, unsigned size, unsigned *offset)
{
    for (unsigned i = 0; i < model_count; ++i) {
        struct model *m = &models[i];
        if (port >= m->port && (unsigned)port + size <= (unsigned)m->port + 256) {
            assert(m->command & 1);
            *offset = port - m->port;
            assert(!(*offset & (size - 1)));
            return m;
        }
    }
    abort();
}

/** @brief Return the bytes of a retained DMA bus address. */
static void *dma(uint64_t physical)
{
    for (unsigned i = 0; i < 8; ++i)
        if (memory[i].physical && physical >= memory[i].physical &&
            physical - memory[i].physical < (uint64_t)memory[i].pages * 4096)
            return (uint8_t *)memory[i].bytes + physical - memory[i].physical;
    return NULL;
}

/** @brief Model register reads without synthesizing TX completion. */
static uint32_t read_port(uint16_t port, unsigned size)
{
    unsigned offset;
    struct model *m = port_model(port, size, &offset);
    if (m->lost)
        return size == 1 ? UINT8_MAX : size == 2 ? UINT16_MAX : UINT32_MAX;
    if (offset == R8139_COMMAND) {
        uint32_t consumer = (word(m->reg + R8139_CAPR, 2) + 16) & (R8139_RX_BYTES - 1);
        return m->reg[offset] | (consumer == m->producer ? R8139_CMD_EMPTY : 0);
    }
    return word(m->reg + offset, size);
}

/** @brief Model reset, W1C status and DMA publication using real port writes. */
static void write_port(uint16_t port, uint32_t value, unsigned size)
{
    unsigned offset;
    struct model *m = port_model(port, size, &offset);
    if (m->lost)
        return;
    if (offset == R8139_ISR) {
        assert(size == 2);
        store(m->reg + offset, word(m->reg + offset, 2) & ~value, 2);
        return;
    }
    if (offset == R8139_COMMAND && (value & R8139_CMD_RESET)) {
        ++m->resets;
        assert(!(m->command & 4));
        if (m->reset_stuck) {
            m->reg[offset] = R8139_CMD_RESET;
            return;
        }
        memset(m->reg + R8139_MAR, 0, sizeof(m->reg) - R8139_MAR);
        m->producer = 0;
        for (unsigned i = 0; i < 4; ++i)
            store(m->reg + R8139_TSD + i * 4, R8139_TX_HOST, 4);
        return;
    }
    if (offset == R8139_COMMAND && (value & (R8139_CMD_RX | R8139_CMD_TX))) {
        assert((m->command & 5) == 5);
        assert(dma(word(m->reg + R8139_RBSTART, 4)));
    }
    if (offset >= R8139_TSD && offset < R8139_TSD + 16) {
        assert(size == 4 && (m->command & 4) && (m->reg[R8139_COMMAND] & R8139_CMD_TX));
        uint32_t previous = word(m->reg + offset, 4);
        assert(previous & R8139_TX_HOST);
        assert((value & 0x1fff) >= 60 && (value & 0x1fff) <= 1518);
        unsigned slot = (offset - R8139_TSD) / 4;
        assert(dma(word(m->reg + R8139_TSAD + slot * 4, 4)));
        ++m->kicks;
    }
    if (offset == R8139_TCR && (value & 1))
        ++m->abort_clears;
    if (offset == R8139_RCR) {
        assert(!(value & 0x80)); /* Real circular RX copying, not extension copying. */
        assert((8192u << ((value >> 11) & 3)) == R8139_RX_BYTES);
        m->producer = 0;
    }
    store(m->reg + offset, value, size);
}

/** @brief Adapt real driver byte reads to the modeled ports. */
static uint8_t in8(uint16_t port) { return read_port(port, 1); }
/** @brief Adapt real driver dword reads to the modeled ports. */
static uint32_t in32(uint16_t port) { return read_port(port, 4); }
/** @brief Adapt real driver byte writes to the modeled ports. */
static void out8(uint16_t port, uint8_t value) { write_port(port, value, 1); }
/** @brief Adapt real driver dword writes to the modeled ports. */
static void out32(uint16_t port, uint32_t value) { write_port(port, value, 4); }
/** @brief Replace only the architectural halfword input instruction. */
uint16_t x86_64_inw(uint16_t port) { return read_port(port, 2); }
/** @brief Replace only the architectural halfword output instruction. */
void x86_64_outw(uint16_t value, uint16_t port) { write_port(port, value, 2); }

/** @brief Model a halfword PCI read. */
static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    struct model *m = pci_model(bus, slot, function);
    if (offset == 4)
        return m->lost ? UINT16_MAX : m->command;
    if (offset == 6)
        return m->pm_cap ? 0x10 : 0;
    return word(m->config + offset, 2);
}

/** @brief Model a halfword PCI write, preserving adjacent status. */
static void pci_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value)
{
    struct model *m = pci_model(bus, slot, function);
    if (offset == 4) {
        if (m->disable_stuck)
            value |= m->command & 4;
        m->command = value;
    } else {
        assert(offset != 6);
        store(m->config + offset, value, 2);
    }
}

/** @brief Model PCI BAR and capability pointer reads. */
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    struct model *m = pci_model(bus, slot, function);
    return offset == 0x10 ? m->bar : word(m->config + offset, 4);
}

/** @brief Enumerate only the real modeled PCI identities. */
static int enumerate(uint32_t index, struct reliefos_driver_pci_device *out)
{
    ++enumerated;
    if (enumeration_error)
        return enumeration_error;
    if (index >= model_count)
        return -19;
    initializing = &models[index];
    *out = initializing->pci;
    return 0;
}

/** @brief Allocate separate CPU and bus addresses and optionally inject failure. */
static uint64_t allocate(uint32_t pages, uint64_t mask)
{
    assert(mask == UINT32_MAX && !(initializing->command & 4));
    if (++attempts == fail_allocation)
        return 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].physical)
            continue;
        uint64_t physical = next_physical + (high_dma ? 0x100000000ULL : 0);
        next_physical += (uint64_t)pages * 4096;
        void *bytes = calloc(pages, 4096);
        assert(bytes);
        memory[i] = (struct allocation){physical, pages, bytes, initializing};
        ++live;
        return physical;
    }
    abort();
}

/** @brief Map an owned run without assuming physical pointers are CPU pointers. */
static void *physical_alias(uint64_t physical)
{
    return attempts == fail_alias ? NULL : dma(physical);
}

/** @brief Reject any DMA release before reset and verified bus-master closure. */
static void release(uint64_t physical, uint32_t pages)
{
    for (unsigned i = 0; i < 8; ++i) {
        if (memory[i].physical != physical)
            continue;
        assert(memory[i].pages == pages);
        struct model *m = memory[i].owner;
        assert(!(m->command & 4) && !m->reg[R8139_COMMAND] && !m->reset_stuck);
        free(memory[i].bytes);
        memory[i] = (struct allocation){0};
        --live;
        return;
    }
    abort();
}

/** @brief Track the real driver's heap-context lifetime. */
void *kernel_malloc(size_t size) { ++heap_live; return calloc(1, size); }
/** @brief Track the real driver's heap-context release. */
void kernel_free(void *pointer) { assert(heap_live); --heap_live; free(pointer); }
/** @brief Assert lock ownership and forbid recursive critical sections. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
/** @brief Assert paired device-lock release. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
/** @brief Return a controllable monotonic watchdog clock. */
static uint64_t ticks(void) { return now; }
/** @brief Account only sleepable initialization and teardown waits. */
static void sleep_ms(uint64_t milliseconds) { (void)milliseconds; ++sleeps; }
/** @brief Keep expected probe diagnostics quiet in the boundary test. */
static void log_message(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/** @brief Record the manager's unsafe-STOP retention request. */
static void teardown_failure(int error) { assert(error < 0); teardown_error = error; }
/** @brief Capture the production callbacks and their per-device contexts. */
int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context, uint32_t *index)
{
    if (registration_error)
        return registration_error;
    assert(registered < 2 && ops->version == RELIEFNT_NETDEV_OPS_VERSION);
    networks[registered] = ops;
    contexts[registered] = context;
    *index = registered++ + 2;
    return 0;
}

static const struct reliefos_driver_kernel_api api = {
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION, .struct_size = sizeof(api),
    .inb = in8, .outb = out8, .inl = in32, .outl = out32,
    .pci_enumerate = enumerate, .pci_read16 = pci_read16, .pci_write16 = pci_write16,
    .pci_read32 = pci_read32, .alloc_dma = allocate, .free_dma = release,
    .phys_to_virt = physical_alias, .ticks = ticks, .sleep_ms = sleep_ms,
    .console_log = log_message, .report_teardown_failure = teardown_failure,
};

/** @brief Start each scenario with independent PCI functions and clean leases. */
static void setup(void)
{
    assert(!live && !heap_live);
    memset(models, 0, sizeof(models));
    memset(networks, 0, sizeof(networks));
    memset(contexts, 0, sizeof(contexts));
    model_count = 1;
    registered = attempts = fail_allocation = fail_alias = enumerated = sleeps = 0;
    registration_error = enumeration_error = teardown_error = 0;
    now = 1;
    next_physical = 0x110000;
    high_dma = false;
    for (unsigned i = 0; i < 2; ++i) {
        struct model *m = &models[i];
        m->pci = (struct reliefos_driver_pci_device){.bus = 0, .slot = i + 3,
            .vendor_id = 0x10ec, .device_id = 0x8139, .class_code = 2};
        m->port = 0xc000 + i * 256;
        m->bar = m->port | 1;
        m->command = 7; /* Firmware bus mastering must not be restored on exit. */
        uint8_t address[6] = {0x52, 0x54, 0, 0x12, 0x34, (uint8_t)(0x56 + i)};
        memcpy(m->reg, address, sizeof(address));
    }
}

/** @brief Require normal STOP to release all device and DMA ownership. */
static void finish(void)
{
    teardown_error = 0;
    rtl8139_driver_module.fini();
    assert(!teardown_error && !live && !heap_live);
    for (unsigned i = 0; i < model_count; ++i)
        assert(!(models[i].command & 4));
}

/** @brief Mark an actual submitted buffer complete in hardware. */
static void complete(struct model *m, unsigned index, uint32_t status)
{
    store(m->reg + R8139_TSD + index * 4, R8139_TX_HOST | status, 4);
}

/** @brief Produce one wrapped classic RX record with a hardware FCS trailer. */
static void inject(struct model *m, const uint8_t *data, unsigned size, uint16_t status)
{
    assert((m->command & 4) && (m->reg[R8139_COMMAND] & R8139_CMD_RX));
    uint8_t *ring = dma(word(m->reg + R8139_RBSTART, 4));
    assert(ring && size >= 60 && size <= 1518);
    uint32_t header = status | (size + 4) << 16;
    for (unsigned i = 0; i < size + 8; ++i) {
        uint8_t value = i < 4 ? (header >> (i * 8)) : i < size + 4 ? data[i - 4] : 0xac;
        ring[(m->producer + i) & (R8139_RX_BYTES - 1)] = value;
    }
    m->producer = ((m->producer + size + 11) & ~3u) & (R8139_RX_BYTES - 1);
    store(m->reg + R8139_ISR, word(m->reg + R8139_ISR, 2) | 1u, 2);
}

/** @brief Cover ABI gates, binding, capabilities, rollback and unsafe STOP. */
static void initialization(void)
{
    assert(rtl8139_driver_module.init(NULL) == -22);
    struct reliefos_driver_kernel_api old = api;
    old.struct_size = 168;
    assert(rtl8139_driver_module.init(&old) == -22);
    setup(); model_count = 0;
    assert(rtl8139_driver_module.init(&api) == -19); finish();
    setup(); models[0].pci.device_id = 0x8168;
    assert(rtl8139_driver_module.init(&api) == -19);
    assert(!attempts && !heap_live); models[0].command &= ~4u; finish();
    setup(); models[0].bar = 0xc081;
    assert(rtl8139_driver_module.init(&api) == -19); finish();
    setup(); models[0].bar = 0x10001;
    assert(rtl8139_driver_module.init(&api) == -19); finish();
    setup(); models[0].bar = 0xfebf0000;
    assert(rtl8139_driver_module.init(&api) == -19); finish();
    setup(); models[0].reg[0] |= 1;
    assert(rtl8139_driver_module.init(&api) == -5); finish();
    setup(); memset(models[0].reg, 0, 6);
    assert(rtl8139_driver_module.init(&api) == -5); finish();
    for (unsigned failure = 1; failure <= 2; ++failure) {
        setup(); fail_allocation = failure;
        assert(rtl8139_driver_module.init(&api) == -12); finish();
        setup(); fail_alias = failure;
        assert(rtl8139_driver_module.init(&api) == -12); finish();
    }
    setup(); high_dma = true;
    assert(rtl8139_driver_module.init(&api) == -5); finish();
    setup(); registration_error = -16;
    assert(rtl8139_driver_module.init(&api) == -16); finish();
    setup(); models[0].reset_stuck = true;
    assert(rtl8139_driver_module.init(&api) == -110);
    assert(heap_live == 1 && !live);
    rtl8139_driver_module.fini(); assert(teardown_error == -110 && heap_live == 1);
    models[0].reset_stuck = false; finish();
    setup(); models[0].pm_cap = true;
    store(models[0].config + 0x34, 0x40, 4);
    store(models[0].config + 0x40, 0x4005, 2); /* Cyclic non-PM capability. */
    assert(rtl8139_driver_module.init(&api) == -22); finish();
    setup(); models[0].pm_cap = true;
    store(models[0].config + 0x34, 0x40, 4);
    store(models[0].config + 0x40, 1, 2);
    store(models[0].config + 0x44, 0x8103, 2);
    assert(!rtl8139_driver_module.init(&api));
    assert(word(models[0].config + 0x44, 2) == 0x100 && sleeps == 1);
    assert(word(models[0].reg + R8139_IMR, 2) == 0 && (models[0].command & 0x400));
    finish();
    puts("PASS rtl8139: ABI/binding, D0/capability bounds, DMA32, rollback and init STOP retention");
}

/** @brief Exercise four-slot ownership, short-frame padding and stalled TX. */
static void transmit(void)
{
    setup(); assert(!rtl8139_driver_module.init(&api));
    struct r8139_device *d = contexts[0];
    const struct reliefnt_netdev_ops *ops = networks[0];
    struct model *m = &models[0];
    uint8_t frame[1518]; memset(frame, 0x93, sizeof(frame));
    assert(ops->send(d, NULL, 60) == -22);
    assert(ops->send(d, frame, 13) == -22 && ops->send(d, frame, 1519) == -22);
    memset(d->tx, 0xff, R8139_TX_PAGES * 4096);
    assert(!ops->send(d, frame, 14));
    assert(!memcmp(d->tx, frame, 14));
    for (unsigned i = 14; i < 60; ++i) assert(!d->tx[i]);
    for (unsigned i = 1; i < 4; ++i) assert(!ops->send(d, frame, sizeof(frame)));
    assert(ops->send(d, frame, 60) == -11 && m->kicks == 4);
    complete(m, 0, 0); /* Host-own alone must not release a still-transmitting slot. */
    assert(ops->send(d, frame, 60) == -11);
    for (unsigned i = 0; i < 4; ++i) complete(m, i, R8139_TX_OK);
    for (unsigned round = 0; round < 128; ++round) {
        unsigned index = d->tx_next;
        assert(!ops->send(d, frame, sizeof(frame)));
        assert(!memcmp(d->tx + index * R8139_TX_BYTES, frame, sizeof(frame)));
        complete(m, index, round == 2 ? R8139_TX_ABORT :
                 round == 3 ? R8139_TX_UNDERRUN : R8139_TX_OK);
    }
    assert(d->tx_errors == 2 && m->abort_clears == 1);
    assert(!ops->send(d, frame, 60));
    now += 5u * RELIEFNT_TICK_HZ;
    unsigned before = m->resets;
    assert(ops->send(d, frame, 60) == -11 && m->resets == before + 1);
    assert(d->recoveries == 1 && live == 2 && !sleeps);
    assert(!ops->send(d, frame, 60));
    m->reg[R8139_MEDIA] = R8139_LINK_BAD;
    assert(!ops->is_ready(d) && ops->send(d, frame, 60) == -100);
    m->reg[R8139_MEDIA] = 0;
    assert(ops->is_ready(d));
    finish();
    puts("PASS rtl8139: TX ownership/wrap/backpressure, zero padding, abort/underrun and watchdog recovery");
}

/** @brief Exercise wrapped payloads, FCS removal, invalid records and recovery. */
static void receive(void)
{
    setup(); assert(!rtl8139_driver_module.init(&api));
    struct r8139_device *d = contexts[0];
    const struct reliefnt_netdev_ops *ops = networks[0];
    struct model *m = &models[0];
    uint8_t sent[1518], received[1520];
    uint32_t length = 123;
    assert(!ops->poll(d, received, sizeof(received), &length) && !length);
    for (unsigned round = 0; round < 256; ++round) {
        unsigned size = 60 + (round * 197) % 1459;
        for (unsigned i = 0; i < size; ++i) sent[i] = (uint8_t)(round * 7 + i * 37);
        memset(received, 0xfd, sizeof(received));
        inject(m, sent, size, R8139_RX_OK);
        assert(ops->poll(d, received, sizeof(received), &length) == 1);
        assert(length == size && !memcmp(sent, received, size));
        assert(received[size] == 0xfd && d->rx_next == m->producer);
    }
    inject(m, sent, 60, R8139_RX_OK);
    assert(ops->poll(d, received, 59, &length) == -90 && !length);
    inject(m, sent, 60, R8139_RX_OK | 4u);
    assert(ops->poll(d, received, sizeof(received), &length) == -74 && !length);
    inject(m, sent, 60, R8139_RX_OK);
    uint32_t position = d->rx_next;
    uint32_t header = word(d->rx + position, 4);
    store(d->rx + position, 0xfff00001u, 4);
    assert(!ops->poll(d, received, sizeof(received), &length) && d->rx_next == position);
    store(d->rx + position, header, 4);
    assert(ops->poll(d, received, sizeof(received), NULL) == 1);
    for (unsigned bad = 0; bad < 3; ++bad) {
        inject(m, sent, 60, R8139_RX_OK);
        store(d->rx + d->rx_next, ((bad == 0 ? 1u : bad == 1 ? 1523u : 65535u) << 16) | 1u, 4);
        assert(ops->poll(d, received, sizeof(received), &length) == -74);
        assert(d->rx_next == 0 && m->producer == 0 && !length);
    }
    store(m->reg + R8139_ISR, R8139_RX_OVERFLOW, 2);
    assert(ops->poll(d, received, sizeof(received), &length) == -74 && d->recoveries == 4);
    inject(m, sent, 60, R8139_RX_OK);
    assert(ops->poll(d, received, sizeof(received), &length) == 1 && length == 60);
    assert(!sleeps && live == 2);
    finish();
    puts("PASS rtl8139: RX wrap/FCS/capacity, DMA-in-flight, bad CRC/length and overflow recovery");
}

/** @brief Confirm failure isolation and retained leases across STOP retries. */
static void lifecycle(void)
{
    uint8_t packet[60] = {0}; uint32_t length;
    setup(); assert(!rtl8139_driver_module.init(&api));
    struct r8139_device *d = contexts[0];
    models[0].reset_stuck = true;
    store(models[0].reg + R8139_ISR, R8139_RX_OVERFLOW, 2);
    assert(networks[0]->poll(d, packet, sizeof(packet), &length) == -110 && d->fault);
    assert(live == 2 && !networks[0]->is_ready(d));
    rtl8139_driver_module.fini(); assert(teardown_error == -110 && live == 2 && heap_live == 1);
    models[0].reset_stuck = false; finish();
    setup(); assert(!rtl8139_driver_module.init(&api));
    models[0].disable_stuck = true;
    rtl8139_driver_module.fini(); assert(teardown_error == -5 && live == 2 && heap_live == 1);
    models[0].disable_stuck = false; finish();
    setup(); assert(!rtl8139_driver_module.init(&api));
    store(models[0].reg + R8139_ISR, R8139_PCI_ERROR, 2);
    assert(networks[0]->poll(contexts[0], packet, sizeof(packet), &length) == -5);
    assert(!networks[0]->is_ready(contexts[0]) && live == 2); finish();
    setup(); model_count = 2;
    assert(!rtl8139_driver_module.init(&api) && registered == 2 && live == 4);
    assert(contexts[0]->rx_pa != contexts[1]->rx_pa && contexts[0]->tx_pa != contexts[1]->tx_pa);
    assert(memcmp(networks[0]->mac(contexts[0]), networks[1]->mac(contexts[1]), 6));
    struct e1000_info identity;
    networks[1]->get_info(contexts[1], &identity);
    assert(identity.vendor_id == 0x10ec && identity.device_id == 0x8139 && identity.slot == 4);
    models[0].reg[R8139_MEDIA] = R8139_LINK_BAD;
    assert(!networks[0]->is_ready(contexts[0]) && networks[1]->is_ready(contexts[1]));
    assert(!networks[1]->send(contexts[1], packet, sizeof(packet)));
    inject(&models[1], packet, sizeof(packet), R8139_RX_OK);
    assert(networks[1]->poll(contexts[1], packet, sizeof(packet), &length) == 1);
    finish();
    puts("PASS rtl8139: failure isolation, failed-reset/busmaster STOP retention, retry and multiple NICs");
}

/** @brief Run the production driver boundary scenarios under ASan and UBSan. */
int main(void)
{
    initialization();
    transmit();
    receive();
    lifecycle();
    return 0;
}

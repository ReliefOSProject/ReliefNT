/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/virtio.h>
#include <reliefnt/pci_irq.h>
#include <reliefnt/export.h>
#include <reliefnt/time.h>
#include <linux/errno.h>

struct virtio_capability { uint8_t bar; uint32_t offset, length; };

/** @brief Read a naturally aligned little-endian common-config halfword.
 * @param d Mapped transport. @param offset Valid common register offset.
 * @return Register contents; caller serializes selected-register accesses.
 */
static uint16_t read16(struct virtio_pci *d, unsigned offset)
{ return *(volatile uint16_t *)(d->common + offset); }
/** @brief Write a naturally aligned common-config halfword.
 * @param d Mapped transport. @param offset Register offset. @param value Contents.
 */
static void write16(struct virtio_pci *d, unsigned offset, uint16_t value)
{ *(volatile uint16_t *)(d->common + offset) = value; }
/** @brief Write a common-config dword without unsupported 64-bit MMIO accesses.
 * @param d Mapped transport. @param offset Register offset. @param value Contents.
 */
static void write32(struct virtio_pci *d, unsigned offset, uint32_t value)
{ *(volatile uint32_t *)(d->common + offset) = value; }
/** @brief Read a selected common-config dword.
 * @param d Mapped transport. @param offset Register offset. @return Contents.
 */
static uint32_t read32(struct virtio_pci *d, unsigned offset)
{ return *(volatile uint32_t *)(d->common + offset); }
/** @brief Copy one PCI configuration dword through the serialized core API.
 * @param pci Retained identity. @param offset Aligned offset. @return Contents.
 */
static uint32_t config32(const struct pci_device *pci, uint8_t offset)
{ return pci_config_read32(pci->bus, pci->slot, pci->function, offset); }

/** @brief Validate and map a complete capability within its exact assigned BAR.
 * @param d Stopped exclusive transport. @param cap Validated capability metadata.
 * @param out Empty lease output. @return Zero or -EINVAL/-ENOMEM.
 * Task context, PCI bus mastering disabled; never probes another function.
 */
static int map_capability(struct virtio_pci *d, const struct virtio_capability *cap,
                           struct virtio_pci_mapping *out)
{
    if (cap->bar > 5 || !cap->length) return -EINVAL;
    /* A BIR must name the low half of a memory BAR, never a 64-bit high half. */
    for (unsigned bar = 0; bar < cap->bar; ++bar) {
        uint32_t lower = config32(&d->pci, 0x10 + bar * 4);
        if (!(lower & 1) && (lower & 6) == 4) {
            if (++bar == cap->bar) return -EINVAL;
        }
    }
    uint32_t lower = config32(&d->pci, 0x10 + cap->bar * 4);
    if ((lower & 1) || (lower & 6) == 2 || (lower & 6) == 6) return -EINVAL;
    uint64_t base = lower & ~15u;
    if ((lower & 6) == 4) {
        if (cap->bar == 5) return -EINVAL;
        base |= (uint64_t)config32(&d->pci, 0x14 + cap->bar * 4) << 32;
    }
    uint64_t offset_end = (uint64_t)cap->offset + cap->length;
    if (!base || offset_end > UINT64_MAX - base ||
        base + offset_end > UINT64_MAX - 4095) return -EINVAL;
    uint64_t start = (base + cap->offset) & ~4095ULL;
    uint64_t end = (base + offset_end + 4095) & ~4095ULL;
    void *mapping = pci_map_device_bar(&d->pci, cap->bar, start, end - start);
    if (!mapping) return -ENOMEM;
    *out = (struct virtio_pci_mapping){ .base = mapping, .bytes = end - start,
        .registers = (volatile uint8_t *)mapping + (base + cap->offset - start),
        .length = cap->length };
    return 0;
}

/** @brief Parse a bounded, cycle-checked conventional PCI capability list.
 * @param d Exclusive transport. @param caps Four required region outputs.
 * @return Zero or -ENODEV/-EINVAL. No MMIO or device DMA is enabled here.
 */
static int capabilities(struct virtio_pci *d, struct virtio_capability caps[4])
{
    if (!(config32(&d->pci, 4) & (1u << 20))) return -ENODEV;
    bool visited[256] = {0};
    uint8_t offset = config32(&d->pci, 0x34) & 0xff;
    for (unsigned n = 0; offset && n < 48; ++n) {
        if (offset < 0x40 || (offset & 3) || visited[offset]) return -EINVAL;
        visited[offset] = true;
        uint32_t header = config32(&d->pci, offset);
        uint8_t next = header >> 8, length = header >> 16, type = header >> 24;
        if ((header & 0xff) == 9 && type >= 1 && type <= 4) {
            if (length < 16 || offset > 256 - length ||
                (type == 2 && length < 20)) return -EINVAL;
            if (!caps[type - 1].length) {
                caps[type - 1] = (struct virtio_capability){
                    .bar = config32(&d->pci, offset + 4) & 0xff,
                    .offset = config32(&d->pci, offset + 8),
                    .length = config32(&d->pci, offset + 12) };
                if (type == 2) d->notify_multiplier = config32(&d->pci, offset + 16);
            }
        }
        offset = next;
        if (!offset) break;
    }
    if (offset) return -EINVAL;
    if (caps[0].length < 56 || caps[1].length < 2 ||
        caps[2].length < 1 || caps[3].length < 1 ||
        (caps[0].offset & 3) || (caps[1].offset & 1) ||
        (d->notify_multiplier & 1)) return -ENODEV;
    return 0;
}

/** @brief Acknowledge the PCI ISR and enter only the retained IRQ-safe consumer.
 * @param context Transport retained until pci_free_irq drains execution pins.
 */
static void interrupt(void *context)
{
    struct virtio_pci *d = context;
    uint8_t status = d->isr ? *d->isr : 0;
    if ((status || d->irq_mode == 1) && d->interrupt) d->interrupt(d->interrupt_context);
}

/** @brief Reset the device and wait until all its virtqueues cease being live.
 * @param d Exclusive mapped transport. @return Zero or -ETIMEDOUT.
 * No DMA/IRQ allocation is freed on failure; task context with bounded polling.
 */
int virtio_pci_reset(struct virtio_pci *d)
{
    if (!d || !d->common) return -EINVAL;
    d->common[20] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t deadline = time_uptime_us() + 1000000;
    for (unsigned count = 0; count < 10000000; ++count) {
        if (!d->common[20]) {
            pci_config_write16(d->pci.bus, d->pci.slot, d->pci.function, 4,
                pci_config_read16(d->pci.bus,d->pci.slot,d->pci.function,4) & ~4u);
            d->live = false;
            if (d->irq_handle) pci_free_irq(d->irq_handle);
            d->irq_handle = 0; d->irq_mode = 0;
            return 0;
        }
        if (time_uptime_us() >= deadline) break;
        __asm__ volatile("pause");
    }
    d->live = true;
    return -ETIMEDOUT;
}

/** @brief Release stopped transport leases after synchronizing IRQ callbacks.
 * @param d Exclusive object; a live/failed-reset device is deliberately retained.
 */
void virtio_pci_close(struct virtio_pci *d)
{
    if (!d || d->live) return;
    if (d->irq_handle) pci_free_irq(d->irq_handle);
    for (unsigned i = 0; i < 4; ++i)
        if (d->mappings[i].base) pci_unmap_mmio(d->mappings[i].base, d->mappings[i].bytes);
    if (d->pci.vendor_id) pci_config_write16(d->pci.bus,d->pci.slot,d->pci.function,4,
        d->original_command & ~4u);
    *d = (struct virtio_pci){0};
}

/** @brief Negotiate exactly supported modern PCI features on one exclusive device.
 * @param d Empty transport. @param pci Identity. @param wanted Optional feature mask.
 * @return Zero or errno; VERSION_1 mandatory, packed/access-platform not claimed.
 */
int virtio_pci_open(struct virtio_pci *d, const struct pci_device *pci, uint64_t wanted)
{
    if (!d || !pci || pci->vendor_id != 0x1af4 || d->common) return -EINVAL;
    d->pci = *pci;
    d->original_command = pci_config_read16(pci->bus,pci->slot,pci->function,4);
    pci_config_write16(pci->bus,pci->slot,pci->function,4,(d->original_command | 2) & ~4u);
    struct virtio_capability caps[4] = {0};
    int rc = capabilities(d, caps);
    if (rc) goto failed;
    for (unsigned i = 0; i < 4; ++i) {
        rc = map_capability(d, &caps[i], &d->mappings[i]);
        if (rc) goto failed;
    }
    d->common = d->mappings[0].registers;
    d->isr = d->mappings[2].registers; d->config = d->mappings[3].registers;
    rc = virtio_pci_reset(d);
    if (rc) return rc;
    d->common[20] = 1; d->common[20] = 3;
    write32(d, 0, 0); uint64_t features = read32(d, 4);
    write32(d, 0, 1); features |= (uint64_t)read32(d, 4) << 32;
    if (!(features & VIRTIO_F_VERSION_1)) { rc = -EOPNOTSUPP; goto failed; }
    d->features = features & (wanted | VIRTIO_F_VERSION_1);
    write32(d, 8, 0); write32(d, 12, d->features);
    write32(d, 8, 1); write32(d, 12, d->features >> 32);
    d->common[20] = 11;
    if (!(d->common[20] & 8)) { rc = -EOPNOTSUPP; goto failed; }
    return 0;
failed:
    if (d->common && virtio_pci_reset(d)) return rc;
    virtio_pci_close(d); return rc;
}

/** @brief Configure a bounded power-of-two split queue before device activation.
 * @param d Negotiated stopped transport. @param q Empty retained queue.
 * @param index Device queue. @param maximum Desired descriptor limit.
 * @return Zero or errno; no queue memory becomes accessible before DRIVER_OK.
 */
int virtio_pci_queue_create(struct virtio_pci *d, struct virtio_queue *q,
                            uint16_t index, uint16_t maximum)
{
    if (!d || !d->common || d->live || !q || q->memory.cpu ||
        index >= read16(d,18) || !maximum) return -EINVAL;
    write16(d,22,index);
    uint16_t size = read16(d,24);
    if (!size || read16(d,28)) return -ENODEV;
    if (size > maximum) size = maximum;
    if (size > VIRTIO_QUEUE_MAX) size = VIRTIO_QUEUE_MAX;
    uint16_t power = 1; while ((unsigned)power * 2 <= size) power *= 2;
    size = power;
    uint64_t notify_offset = (uint64_t)read16(d,30) * d->notify_multiplier;
    if (notify_offset > d->mappings[1].length - 2u) return -EINVAL;
    int rc = dma_buffer_alloc(&q->memory, (size_t)size * 32 + 4096, UINT64_MAX);
    if (rc) return rc;
    rc = virtio_queue_bind(q,q->memory.cpu,q->memory.physical,q->memory.capacity,size);
    if (rc) { (void)dma_buffer_free(&q->memory); return rc; }
    q->index = index;
    q->notify = (void *)(d->mappings[1].registers + notify_offset);
    q->available[0] = d->irq_mode ? 0 : 1;
    write16(d,24,size);
    write16(d,26,d->irq_mode == 1 ? 0 : 0xffff);
    if (d->irq_mode == 1 && read16(d,26) != 0) { rc = -EIO; goto failed; }
    write32(d,32,q->descriptor_physical); write32(d,36,q->descriptor_physical >> 32);
    write32(d,40,q->available_physical); write32(d,44,q->available_physical >> 32);
    write32(d,48,q->used_physical); write32(d,52,q->used_physical >> 32);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    write16(d,28,1);
    return 0;
failed:
    (void)dma_buffer_free(&q->memory); *q = (struct virtio_queue){0}; return rc;
}

/** @brief Prefer a shared MSI-X vector, then MSI, leaving polling supported.
 * @param d Stopped transport. @param handler IRQ-safe consumer.
 * @param context Caller-owned stable context; no execution lock in callback.
 */
void virtio_pci_interrupt_prepare(struct virtio_pci *d, void (*handler)(void *), void *context)
{
    d->interrupt = handler; d->interrupt_context = context;
    struct reliefos_driver_pci_device pci = { .bus=d->pci.bus,.slot=d->pci.slot,
        .function=d->pci.function,.vendor_id=d->pci.vendor_id,.device_id=d->pci.device_id };
    if (!pci_request_msix_irq(&pci,interrupt,d,&d->irq_handle)) {
        d->irq_mode = 1; write16(d,16,0);
        if (read16(d,16) == 0) return;
        pci_free_irq(d->irq_handle); d->irq_handle = 0; d->irq_mode = 0;
    }
    if (!pci_request_irq(&pci,interrupt,d,&d->irq_handle)) d->irq_mode = 2;
}

/** @brief Publish queue initialization before enabling PCI DMA and DRIVER_OK.
 * @param d Negotiated stopped device. @return Zero or -EIO on device failure.
 */
int virtio_pci_ready(struct virtio_pci *d)
{
    if (!d || !d->common || (d->common[20] & 0xcf) != 11) return -EIO;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pci_config_write16(d->pci.bus,d->pci.slot,d->pci.function,4,
        pci_config_read16(d->pci.bus,d->pci.slot,d->pci.function,4) | 6);
    d->common[20] = 15; d->live = true;
    return (d->common[20] & 0xc0) ? -EIO : 0;
}

/** @brief Reclaim coherent ring pages after a successful device reset.
 * @param d Stopped transport. @param q Owned ring.
 * @return Zero or -EBUSY; a failed-reset device never frees queue memory.
 */
int virtio_pci_queue_destroy(struct virtio_pci *d, struct virtio_queue *q)
{
    if (!d || !q || d->live) return -EBUSY;
    int rc = dma_buffer_free(&q->memory);
    if (!rc) *q = (struct virtio_queue){0};
    return rc;
}

EXPORT_SYMBOL(virtio_pci_open);
EXPORT_SYMBOL(virtio_pci_queue_create);
EXPORT_SYMBOL(virtio_pci_interrupt_prepare);
EXPORT_SYMBOL(virtio_pci_ready);
EXPORT_SYMBOL(virtio_pci_reset);
EXPORT_SYMBOL(virtio_pci_close);
EXPORT_SYMBOL(virtio_pci_queue_destroy);
EXPORT_SYMBOL(virtio_queue_submit);
EXPORT_SYMBOL(virtio_queue_pop);
EXPORT_SYMBOL(virtio_queue_reserve_emergency);
EXPORT_SYMBOL(virtio_queue_emergency);

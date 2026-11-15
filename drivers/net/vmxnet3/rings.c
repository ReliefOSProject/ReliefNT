/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Store one aligned x86 little-endian word in coherent DMA memory.
 * @param p Allocation base. @param offset Byte offset. @param value Value to store.
 */
static void put32(uint8_t *p, unsigned offset, uint32_t value)
{
    *(uint32_t *)(p + offset) = value;
}
/** @brief Store one aligned x86 little-endian bus address in coherent DMA memory.
 * @param p Allocation base. @param offset Byte offset. @param value Value to store.
 */
static void put64(uint8_t *p, unsigned offset, uint64_t value)
{
    *(uint64_t *)(p + offset) = value;
}

/** @brief Allocate coherent shared memory, rings and packet buffers.
 * @param d Negotiated device. @return Zero or -ENOMEM; partial allocations remain owned.
 */
int vmx_allocate(struct vmx_device *d)
{
    d->shared_pa = d->api->alloc_dma(1, UINT64_MAX);
    d->rings_pa = d->api->alloc_dma(VMX_RING_PAGES, UINT64_MAX);
    d->tx_data_pa = d->api->alloc_dma(VMX_DATA_PAGES, UINT64_MAX);
    d->rx_data_pa = d->api->alloc_dma(VMX_DATA_PAGES, UINT64_MAX);
    if (!d->shared_pa || !d->rings_pa || !d->tx_data_pa || !d->rx_data_pa)
        return -12;
    d->shared = d->api->phys_to_virt(d->shared_pa);
    uint8_t *rings = d->api->phys_to_virt(d->rings_pa);
    d->tx_data = d->api->phys_to_virt(d->tx_data_pa);
    d->rx_data = d->api->phys_to_virt(d->rx_data_pa);
    if (!d->shared || !rings || !d->tx_data || !d->rx_data)
        return -12;
    d->tx = (volatile struct vmx_desc *)(rings + VMX_TX_OFFSET);
    d->txc = (volatile struct vmx_desc *)(rings + VMX_TXC_OFFSET);
    d->rx = (volatile struct vmx_desc *)(rings + VMX_RX_OFFSET);
    d->rxc = (volatile struct vmx_desc *)(rings + VMX_RXC_OFFSET);
    d->tx_gen = d->txc_gen = d->rx_gen = d->rxc_gen = 1;
    d->rx_fill = VMX_RX_COUNT - 1;
    for (unsigned i = 0; i < VMX_RX_COUNT; ++i) {
        d->rx[i].address = d->rx_data_pa + (uint64_t)i * VMX_BUFFER_BYTES;
        d->rx[i].a = VMX_BUFFER_BYTES | (i + 1 < VMX_RX_COUNT ? VMX_GEN : 0);
        d->rx[i].b = 0;
    }
    return 0;
}

/** @brief Populate revision 1 shared configuration and activate the device.
 * @param d Allocated context. @return Zero or -EIO on rejected activation.
 */
int vmx_activate(struct vmx_device *d)
{
    uint8_t *s = d->shared, *q = s + VMX_QUEUE_OFFSET;
    /* Device-visible shared offsets follow VMXNET3 revision 1 ABI. */
    put32(s, 0, 0xbabefee1u);
    put32(s, 8, 1);
    put32(s, 12, 6); /* 64-bit guest, Linux ABI */
    put32(s, 16, 1);
    put32(s, 20, 1); /* revision and UPT version */
    put64(s, 40, d->shared_pa + VMX_QUEUE_OFFSET);
    put32(s, 52, 512);
    put32(s, 56, 1500);
    s[60] = 1;
    s[62] = 1;
    s[63] = 1; /* one fragment and one queue each */
    s[80] = d->irq_handle && d->auto_mask;
    s[81] = 1;
    s[83] = 8; /* UPT adaptive interrupt moderation. */
    put32(s, 108, d->irq_handle ? 0 : 1); /* Poll-only fallback stays globally masked. */
    put32(s, 120, 0x0d); /* unicast, broadcast and all multicast */
    for (unsigned i = 0; i < 128; ++i)
        put32(s, 136 + i * 4, UINT32_MAX);
    /* TX queue at table offset 0; RX queue at offset 256. */
    put64(q, 16, d->rings_pa + VMX_TX_OFFSET);
    put64(q, 32, d->rings_pa + VMX_TXC_OFFSET);
    put64(q, 40, UINT64_MAX);
    put32(q, 56, VMX_TX_COUNT);
    put32(q, 64, VMX_TX_COUNT);
    q += 256;
    put64(q, 16, d->rings_pa + VMX_RX_OFFSET);
    put64(q, 24, d->rings_pa + VMX_RX_BODY_OFFSET);
    put64(q, 32, d->rings_pa + VMX_RXC_OFFSET);
    put64(q, 40, UINT64_MAX);
    put32(q, 56, VMX_RX_COUNT);
    put32(q, 60, VMX_RX_BODY_COUNT);
    put32(q, 64, VMX_RXC_COUNT);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint16_t pci_cmd = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->api->pci_write16(p->bus, p->slot, p->function, 4, pci_cmd | 6u);
    vmx_io_write(d->bar[1], VMX_DS_LOW, (uint32_t)d->shared_pa);
    vmx_io_write(d->bar[1], VMX_DS_HIGH, (uint32_t)(d->shared_pa >> 32));
    d->exposed = true;
    vmx_io_write(d->bar[1], VMX_COMMAND, VMX_ACTIVATE);
    if (vmx_io_read(d->bar[1], VMX_COMMAND))
        return -5;
    vmx_io_write(d->bar[0], VMX_RX_PROD, d->rx_fill);
    vmx_io_write(d->bar[0], VMX_RX_BODY_PROD, 0);
    vmx_io_write(d->bar[1], VMX_COMMAND, VMX_UPDATE_RX_MODE);
    return 0;
}

/** @brief Free all retained DMA buffers after successful hardware stop.
 * @param d Stopped or partially initialized context.
 */
void vmx_free(struct vmx_device *d)
{
    if (d->shared_pa)
        d->api->free_dma(d->shared_pa, 1);
    if (d->rings_pa)
        d->api->free_dma(d->rings_pa, VMX_RING_PAGES);
    if (d->tx_data_pa)
        d->api->free_dma(d->tx_data_pa, VMX_DATA_PAGES);
    if (d->rx_data_pa)
        d->api->free_dma(d->rx_data_pa, VMX_DATA_PAGES);
    d->shared_pa = d->rings_pa = d->tx_data_pa = d->rx_data_pa = 0;
}

/** @brief Prevent use of both queues after a corrupt completion.
 * @param d Device with an untrustworthy queue. @return Negative EIO.
 */
static int broken(struct vmx_device *d)
{
    __atomic_store_n(&d->broken, 1, __ATOMIC_RELAXED);
    return -5;
}

/** @brief Reclaim ordered TX completions without waiting for device progress.
 * @param d Active retained context; caller holds tx_lock with IRQs saved.
 * @return Zero or negative EIO after marking a corrupt queue unusable.
 */
static int reclaim_tx_locked(struct vmx_device *d)
{
    while (d->tx_clean != d->tx_next) {
        volatile struct vmx_desc *c = &d->txc[d->txc_next];
        uint32_t last = c->b;
        if ((last >> 31) != d->txc_gen)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        unsigned index = (unsigned)(c->address & 0xfffu), type = (last >> 24) & 0x7fu;
        if (type || index != d->tx_clean) {
            return broken(d);
        }
        d->tx_clean = (d->tx_clean + 1) % VMX_TX_COUNT;
        if (++d->txc_next == VMX_TX_COUNT) {
            d->txc_next = 0;
            d->txc_gen ^= 1;
        }
    }
    return 0;
}

/** @brief Reclaim TX completions even when the next workload is receive-only.
 * @param d Active retained controller. @return Zero or negative EIO.
 */
int vmx_reclaim_tx(struct vmx_device *d)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->tx_lock,&flags);
    int result=reclaim_tx_locked(d);
    kernel_spin_unlock_irqrestore(&d->tx_lock,flags);
    return result;
}

/** @brief Submit one frame and reclaim already completed descriptors without waiting.
 * @param d Active retained device. @param frame Borrowed Ethernet bytes.
 * @param length Frame bytes. @return Zero or negative Linux validation/queue errno.
 */
int vmx_send(struct vmx_device *d, const void *frame, uint32_t length)
{
    if (!frame || !length) return -22;
    if (length>VMX_FRAME_MAX) return -90;
    if (__atomic_load_n(&d->broken,__ATOMIC_RELAXED)) return -5;
    if (!vmx_link(d)) return -100;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->tx_lock,&flags);
    int result=reclaim_tx_locked(d);
    if (result) { kernel_spin_unlock_irqrestore(&d->tx_lock,flags); return result; }
    unsigned index = d->tx_next, next = (index + 1) % VMX_TX_COUNT;
    if (next == d->tx_clean) {
        kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
        return -11;
    }
    uint8_t *buffer = d->tx_data + index * VMX_BUFFER_BYTES;
    const uint8_t *src = frame;
    for (unsigned i = 0; i < length; ++i)
        buffer[i] = src[i];
    d->tx[index].address = d->tx_data_pa + (uint64_t)index * VMX_BUFFER_BYTES;
    d->tx[index].b = (1u << 12) | (1u << 13); /* EOP, completion requested; no offloads. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->tx[index].a = length | (d->tx_gen << 14);
    d->tx_next = next;
    if (!next)
        d->tx_gen ^= 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vmx_io_write(d->bar[0], VMX_TX_PROD, next);
    kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
    return 0;
}

/** @brief Consume and replenish one RX buffer after validating its completion.
 * @param d Active context. @param frame Writable buffer. @param capacity Buffer bytes.
 * @param length Optional output length, cleared on failure. @return One, zero or negative errno.
 */
int vmx_receive(struct vmx_device *d, void *frame, uint32_t capacity, uint32_t *length)
{
    if (length)
        *length = 0;
    if (!frame || !capacity)
        return -22;
    if (__atomic_load_n(&d->broken, __ATOMIC_RELAXED))
        return -5;
    if (!d->active)
        return -19;
    if (vmx_interrupt_events(d) || vmx_reclaim_tx(d)) return -5;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->rx_lock, &flags);
    volatile struct vmx_desc *c = &d->rxc[d->rxc_next];
    uint32_t last = c->b;
    if ((last >> 31) != d->rxc_gen) {
        vmx_interrupt_complete(d);
        kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t first = (uint32_t)c->address, info = c->a;
    unsigned index = first & 0xfffu, queue = (first >> 16) & 0x3ffu, type = (last >> 24) & 0x7fu;
    /* QEMU's legacy model leaves type zero; native revision 1 uses type 3. */
    if (queue || index != d->rx_next || (type != 0 && type != 3)) {
        int rc = broken(d);
        kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
        return rc;
    }
    uint32_t bytes = info & 0x3fffu;
    int rc = 1;
    if (bytes > capacity || bytes > VMX_FRAME_MAX)
        rc = -90;
    else if (bytes < 14 || (first & (3u << 14)) != (3u << 14) || (info & (3u << 14)))
        rc = -74;
    if (rc == 1) {
        const uint8_t *source = d->rx_data + index * VMX_BUFFER_BYTES;
        uint8_t *out = frame;
        for (unsigned i = 0; i < bytes; ++i)
            out[i] = source[i];
        if (length)
            *length = bytes;
    }
    d->rx_next = (index + 1) % VMX_RX_COUNT;
    /* Keep one slot unposted; refill that slot, then advance its generation. */
    unsigned fill = d->rx_fill;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->rx[fill].a = VMX_BUFFER_BYTES | (d->rx_gen << 31);
    if (++d->rx_fill == VMX_RX_COUNT) {
        d->rx_fill = 0;
        d->rx_gen ^= 1;
    }
    if (++d->rxc_next == VMX_RXC_COUNT) {
        d->rxc_next = 0;
        d->rxc_gen ^= 1;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* Device-owned updateRxProd requests doorbells only when it needs them. */
    if (*(volatile uint8_t *)(d->shared+VMX_QUEUE_OFFSET+256))
        vmx_io_write(d->bar[0], VMX_RX_PROD, d->rx_fill);
    kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
    return rc;
}

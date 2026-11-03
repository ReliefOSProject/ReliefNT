/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Allocate coherent physical descriptor and packet buffers for both queues.
 * @param d Stopped device retaining each successful physical lease for cleanup.
 * @return Zero or -ENOMEM; descriptor addresses are bus addresses, never CPU aliases.
 */
int e1k_allocate_rings(struct e1k_device *d)
{
    d->rx_physical=d->api->alloc_dma(1,UINT64_MAX);
    d->tx_physical=d->api->alloc_dma(1,UINT64_MAX);
    d->rx_data_physical=d->api->alloc_dma(E1K_RX_PAGES,UINT64_MAX);
    d->tx_data_physical=d->api->alloc_dma(E1K_TX_PAGES,UINT64_MAX);
    if (!d->rx_physical || !d->tx_physical || !d->rx_data_physical || !d->tx_data_physical)
        return -12;
    d->rx=d->api->phys_to_virt(d->rx_physical);
    d->tx=d->api->phys_to_virt(d->tx_physical);
    d->rx_data=d->api->phys_to_virt(d->rx_data_physical);
    d->tx_data=d->api->phys_to_virt(d->tx_data_physical);
    if (!d->rx || !d->tx || !d->rx_data || !d->tx_data) return -12;
    for (unsigned i=0;i<E1K_RX_COUNT;++i)
        d->rx[i].address=d->rx_data_physical+(uint64_t)i*E1K_BUFFER;
    for (unsigned i=0;i<E1K_TX_COUNT;++i) {
        d->tx[i].address=d->tx_data_physical+(uint64_t)i*E1K_BUFFER;
        d->tx[i].status=1;
    }
    return 0;
}

/** @brief Program legacy descriptor queues while receive/transmit are disabled.
 * @param d Device with complete coherent DMA storage and owned MMIO.
 */
void e1k_enable_rings(struct e1k_device *d)
{
    e1k_write(d,E1K_IMC,0xffffffffu);
    (void)e1k_read(d,E1K_ICR);
    e1k_write(d,E1K_PBA,20); /* 20 KiB receive, remaining 20 KiB transmit. */
    for (unsigned i=0;i<128;++i) e1k_write(d,E1K_MTA+i*4,0);
    /* RAR15 belongs to manageability firmware, not the host driver. */
    for (unsigned i=1;i<15;++i) {
        e1k_write(d,E1K_RAL+i*8,0); e1k_write(d,E1K_RAH+i*8,0);
    }
    e1k_write(d,E1K_RXCSUM,0);
    e1k_write(d,E1K_RFCTL,0); /* Legacy descriptors; software validates IP/L4 checksums. */
    e1k_write(d,0x5818,0); /* MRQC: single receive queue, no RSS. */
    e1k_write(d,E1K_RX_BASE,(uint32_t)d->rx_physical);
    e1k_write(d,E1K_RX_BASE+4,(uint32_t)(d->rx_physical>>32));
    e1k_write(d,E1K_RX_BASE+8,E1K_RX_COUNT*sizeof(*d->rx));
    e1k_write(d,E1K_RX_BASE+0x10,0);
    e1k_write(d,E1K_RX_BASE+0x28,(1u<<24)|(1u<<16));
    e1k_write(d,E1K_TX_BASE,(uint32_t)d->tx_physical);
    e1k_write(d,E1K_TX_BASE+4,(uint32_t)(d->tx_physical>>32));
    e1k_write(d,E1K_TX_BASE+8,E1K_TX_COUNT*sizeof(*d->tx));
    e1k_write(d,E1K_TX_BASE+0x10,0);
    e1k_write(d,E1K_TX_BASE+0x18,0);
    uint32_t tx_policy=(1u<<24)|(1u<<16);
    if (d->pci.device_id==0x10d3) tx_policy|=1u<<22; /* 82574 required reserved-bit value. */
    e1k_write(d,E1K_TX_BASE+0x28,tx_policy);
    d->rx_next=d->tx_next=d->tx_clean=0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e1k_write(d,E1K_RX_BASE+0x18,E1K_RX_COUNT-1);
    e1k_write(d,E1K_TIPG,8u|(2u<<10)|(10u<<20));
    e1k_write(d,E1K_TCTL,(1u<<1)|(1u<<3)|(15u<<4)|(63u<<12));
    /* Receive own unicast, all multicast (including IPv6), and broadcasts. */
    e1k_write(d,E1K_RCTL,(1u<<1)|(1u<<4)|(1u<<15)|(1u<<26));
    e1k_flush(d);
}

/** @brief Release exactly the retained allocations after confirmed DMA STOP.
 * @param d Stopped device, including partial initialization; no callbacks remain.
 */
void e1k_free_rings(struct e1k_device *d)
{
    if (d->rx_physical) d->api->free_dma(d->rx_physical,1);
    if (d->tx_physical) d->api->free_dma(d->tx_physical,1);
    if (d->rx_data_physical) d->api->free_dma(d->rx_data_physical,E1K_RX_PAGES);
    if (d->tx_data_physical) d->api->free_dma(d->tx_data_physical,E1K_TX_PAGES);
    d->rx_physical=d->tx_physical=d->rx_data_physical=d->tx_data_physical=0;
    d->rx=NULL; d->tx=NULL; d->rx_data=d->tx_data=NULL;
}

/** @brief Copy and enqueue a frame, bounding work to at most one TX ring.
 * @param d Pinned active device. @param frame Borrowed Ethernet bytes.
 * @param length Bytes excluding FCS, between 1 and E1K_MAX_FRAME.
 * @return Zero if queued, -EINVAL/-EMSGSIZE/-ENETDOWN or -EAGAIN. IRQ-safe;
 * no sleeping, allocation or hardware-completion wait under the ring lock.
 */
int e1k_send(struct e1k_device *d, const void *frame, uint32_t length)
{
    if (!frame || !length) return -22;
    if (length>E1K_MAX_FRAME) return -90;
    if (!d->active || !(e1k_read(d,E1K_STATUS)&E1K_STATUS_LINK)) return -100;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->tx_lock,&flags);
    while (d->tx_clean!=d->tx_next && (d->tx[d->tx_clean].status&1)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        d->tx_clean=(d->tx_clean+1)%E1K_TX_COUNT;
    }
    unsigned index=d->tx_next, next=(index+1)%E1K_TX_COUNT;
    if (next==d->tx_clean) {
        kernel_spin_unlock_irqrestore(&d->tx_lock,flags);
        return -11;
    }
    uint8_t *buffer=d->tx_data+index*E1K_BUFFER;
    const uint8_t *input=frame;
    for (unsigned i=0;i<length;++i) buffer[i]=input[i];
    d->tx[index].length=(uint16_t)length;
    d->tx[index].checksum_start=0;
    d->tx[index].command=0x0b; /* EOP | IFCS | RS, no checksum/segmentation offload. */
    d->tx[index].status=0;
    d->tx[index].checksum_offset=0; d->tx[index].special=0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->tx_next=next;
    e1k_write(d,E1K_TX_BASE+0x18,next);
    kernel_spin_unlock_irqrestore(&d->tx_lock,flags);
    return 0;
}

/** @brief Consume one RX descriptor, rejecting malformed and fragmented frames.
 * @param d Active pinned device. @param frame Writable capacity-byte output.
 * @param capacity Output buffer size. @param length Optional output, reset on error/empty.
 * @return One for a complete frame, zero if empty, or negative input/size/frame errno.
 * IRQ-safe ring lock; no sleeping/allocation. Whole fragment chains are dropped.
 */
int e1k_receive(struct e1k_device *d, void *frame, uint32_t capacity, uint32_t *length)
{
    if (length) *length=0;
    if (!frame || !capacity) return -22;
    if (!d->active) return -19;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->rx_lock,&flags);
    unsigned index=d->rx_next;
    uint8_t status=d->rx[index].status;
    if (!(status&1)) {
        kernel_spin_unlock_irqrestore(&d->rx_lock,flags);
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t bytes=d->rx[index].length;
    int rc=1;
    if (bytes>capacity || bytes>E1K_MAX_FRAME) rc=-90;
    else if (bytes<14 || d->dropping_fragments || !(status&2) ||
             (d->rx[index].errors&0x9f)) rc=-74;
    d->dropping_fragments=!(status&2);
    if (rc==1) {
        const uint8_t *buffer=d->rx_data+index*E1K_BUFFER;
        uint8_t *output=frame;
        for (unsigned i=0;i<bytes;++i) output[i]=buffer[i];
        if (length) *length=bytes;
    }
    d->rx[index].length=0; d->rx[index].checksum=0;
    d->rx[index].status=0; d->rx[index].errors=0; d->rx[index].special=0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e1k_write(d,E1K_RX_BASE+0x18,index);
    d->rx_next=(index+1)%E1K_RX_COUNT;
    kernel_spin_unlock_irqrestore(&d->rx_lock,flags);
    return rc;
}

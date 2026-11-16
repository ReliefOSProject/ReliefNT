/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VIRTIO_H
#define RELIEFNT_VIRTIO_H
#include <reliefnt/dma.h>
#include <reliefnt/lock.h>
#include <reliefnt/pci.h>

#define VIRTIO_QUEUE_MAX 256u
#define VIRTIO_F_VERSION_1 (1ULL << 32)

struct virtio_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};
struct virtio_used_element { uint32_t id, length; };
struct virtio_segment { uint64_t physical; uint32_t length; bool writable; };
struct virtio_queue {
    struct kernel_spinlock lock;
    struct dma_buffer memory;
    volatile struct virtio_descriptor *descriptors;
    volatile uint16_t *available, *used_header, *notify;
    volatile struct virtio_used_element *used_ring;
    uint64_t descriptor_physical, available_physical, used_physical;
    void *cookies[VIRTIO_QUEUE_MAX];
    uint32_t writable_bytes[VIRTIO_QUEUE_MAX];
    uint16_t chain_lengths[VIRTIO_QUEUE_MAX];
    uint16_t size, index, free_head, free_count, pending;
    uint16_t available_index, used_index;
    bool broken, emergency_reserved;
};
struct virtio_pci_mapping { void *base; uint64_t bytes; volatile uint8_t *registers; uint32_t length; };
struct virtio_pci {
    struct pci_device pci;
    struct virtio_pci_mapping mappings[4];
    volatile uint8_t *common, *config, *isr;
    uint64_t features;
    uint32_t notify_multiplier, irq_handle;
    uint16_t original_command;
    uint8_t irq_mode;
    bool live;
    void (*interrupt)(void *);
    void *interrupt_context;
};

/** @brief Initialize a stopped split ring in caller-owned coherent DMA storage.
 * @param queue Empty exclusive object. @param memory Aligned writable bytes.
 * @param physical DMA address of memory. @param bytes Memory capacity.
 * @param size Power-of-two descriptor count, at most VIRTIO_QUEUE_MAX.
 * @return Zero or -EINVAL. No allocation or ownership transfer; task context.
 */
int virtio_queue_bind(struct virtio_queue *queue, void *memory, uint64_t physical,
                       size_t bytes, uint16_t size);
/** @brief Publish one readable-then-writable descriptor chain to a live device.
 * @param queue Live ring retained through completion/reset. @param segments
 * Copied DMA extents; their memory must outlive the request. @param count Extents.
 * @param cookie Non-NULL opaque completion identity, never dereferenced here.
 * @return Zero, -EINVAL, -ENOSPC or -EIO. IRQ-safe short lock; no allocation.
 * Success transfers DMA access until pop returns this cookie or device reset.
 */
int virtio_queue_submit(struct virtio_queue *queue, const struct virtio_segment *segments,
                         unsigned count, void *cookie);
/** @brief Consume one device completion and reclaim exactly its descriptor chain.
 * @param queue Live ring. @param cookie Required output. @param length Device bytes.
 * @return One, zero if empty, or -EIO on malformed completion; broken rings retain
 * all outstanding DMA until reset. IRQ-safe lock; callbacks run outside this API.
 */
int virtio_queue_pop(struct virtio_queue *queue, void **cookie, uint32_t *length);
/** @brief Reserve two descriptors before a split queue becomes device-visible.
 * @param queue Bound, empty and stopped ring with at least four descriptors.
 * @return Zero or -EINVAL; task context, no allocation or DMA publication.
 */
int virtio_queue_reserve_emergency(struct virtio_queue *queue);
/** @brief Exchange one preallocated fatal command through reserved descriptors.
 * @param queue Live reserved ring. @param command Read-only coherent DMA packet.
 * @param response Writable coherent DMA response. @param length Used byte output.
 * @return Zero or -EINVAL/-EIO/-ETIMEDOUT; bounded polling, no locks or allocation.
 * Fatal owner only, interrupts disabled and other CPUs parked; normal queue
 * execution must never resume. This deliberately does not retire normal cookies.
 */
int virtio_queue_emergency(struct virtio_queue *queue,struct virtio_segment command,
                           struct virtio_segment response,uint32_t *length);
/** @brief Reset and negotiate the modern PCI interface of an unclaimed function.
 * @param device Empty owned transport. @param pci Exact exclusive identity.
 * @param wanted Optional device features; VERSION_1 is always mandatory.
 * @return Zero or negative errno; task context. Failure drains/unmaps if reset
 * succeeds; a failed reset retains mappings and must not be discarded.
 */
int virtio_pci_open(struct virtio_pci *device, const struct pci_device *pci, uint64_t wanted);
/** @brief Allocate/configure one disabled queue, before DRIVER_OK.
 * @param device Negotiated stopped transport. @param queue Empty owned ring.
 * @param index Device queue index. @param maximum Desired bounded queue size.
 * @return Zero or errno; coherent backing belongs to queue until device reset.
 */
int virtio_pci_queue_create(struct virtio_pci *device, struct virtio_queue *queue,
                            uint16_t index, uint16_t maximum);
/** @brief Route all queues/config to MSI-X entry zero, MSI, or polling fallback.
 * @param device Negotiated transport before DRIVER_OK. @param handler IRQ-safe
 * nonblocking handler. @param context Retained caller context until close.
 * Task context; no user callback is invoked while a transport lock is held.
 */
void virtio_pci_interrupt_prepare(struct virtio_pci *device, void (*handler)(void *), void *context);
/** @brief Enable DMA and DRIVER_OK after all queue contents have been published.
 * @param device Negotiated transport with configured queues. @return Zero or -EIO.
 */
int virtio_pci_ready(struct virtio_pci *device);
/** @brief Stop all queues before callers release any DMA or optional code.
 * @param device Exclusive transport. @return Zero only after status reads zero;
 * -ETIMEDOUT retains mappings, IRQ and backing. Task context, bounded poll.
 */
int virtio_pci_reset(struct virtio_pci *device);
/** @brief Drain interrupts and unmap an already successfully reset transport.
 * @param device Stopped exclusive object. No-op if live; task context.
 */
void virtio_pci_close(struct virtio_pci *device);
/** @brief Free a ring only after its device reset has been observed.
 * @param device Reset transport. @param queue Owned ring, consumed on success.
 * @return Zero or -EBUSY if DMA may still be live; task context.
 */
int virtio_pci_queue_destroy(struct virtio_pci *device, struct virtio_queue *queue);
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VIRTIO_BLK_INTERNAL_H
#define RELIEFNT_VIRTIO_BLK_INTERNAL_H

#include <reliefnt/blkdev.h>
#include <reliefnt/virtio.h>

#define VIRTIO_BLK_VENDOR 0x1af4u
#define VIRTIO_BLK_DEVICE 0x1042u
#define VIRTIO_BLK_TRANSITIONAL 0x1001u
#define VIRTIO_BLK_SECTOR_SIZE 512u
#define VIRTIO_BLK_MAX_BLOCK_SIZE 4096u
#define VIRTIO_BLK_MAX_BYTES (128u * 1024u)
#define VIRTIO_BLK_DATA_OFFSET 64u
#define VIRTIO_BLK_F_SIZE_MAX (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX (1ULL << 2)
#define VIRTIO_BLK_F_RO (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH (1ULL << 9)
#define VIRTIO_BLK_FEATURES (VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX | \
    VIRTIO_BLK_F_RO | VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH)

struct virtio_blk {
    struct virtio_pci transport;
    struct virtio_queue queue;
    struct dma_buffer dma;
    struct block_device_registration registration;
    struct kernel_spinlock lock;
    uint64_t pending_first, pending_at_tick;
    uint32_t pending_count, pending_pid;
    uint8_t config_generation;
    bool online, pending;
};

/* Probe owns the unpublished object. Read an OASIS config-generation snapshot,
 * then set immutable registry geometry. Does not enable or allocate DMA. */
int virtio_blk_configure(struct virtio_blk *disk);
/* Stop accepting work, then wait for reset. Failure retains every DMA/IRQ lease. */
int virtio_blk_stop(void *context);
extern const struct block_device_operations virtio_blk_operations;

#endif

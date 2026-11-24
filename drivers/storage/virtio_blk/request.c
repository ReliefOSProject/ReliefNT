/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <linux/errno.h>
#include <reliefnt/console.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>

/* Modern PCI common status and config-generation registers, and Device ID 2
 * config fields. The wire data is little-endian, as is ReliefNT x86-64. */
enum { COMMON_STATUS = 20, COMMON_GENERATION = 21 };
enum { STATUS_DRIVER_OK = 4, STATUS_NEEDS_RESET = 64, STATUS_FAILED = 128 };
enum { BLK_IN = 0, BLK_OUT = 1, BLK_FLUSH = 4 };
enum { BLK_OK = 0, BLK_IOERR = 1, BLK_UNSUPP = 2 };
#define BLK_TIMEOUT_SECONDS 5u
#define BLK_TIMEOUT_US (BLK_TIMEOUT_SECONDS * 1000000ULL)
#define BLK_TIMEOUT_SPINS 80000000u

struct blk_header {
    uint32_t type, reserved;
    uint64_t sector;
};
_Static_assert(sizeof(struct blk_header) == 16, "VirtIO-Block request header");

struct blk_geometry {
    uint64_t blocks;
    uint32_t block_size, max_blocks;
    uint8_t generation;
};

/* Existing storage execution context permits only read replay before mutation. */
extern bool storage_async_can_yield(void);

static uint32_t config32(const struct virtio_pci *transport, unsigned offset)
{
    return *(const volatile uint32_t *)(transport->config + offset);
}

static int read_geometry(const struct virtio_pci *d, struct blk_geometry *out)
{
    unsigned length = d->mappings[3].length;
    if (!d->common || !d->config || ((uintptr_t)d->config & 3u) || length < 8 || (length < 24 && (d->features & VIRTIO_BLK_F_BLK_SIZE)) ||
        (length < 16 && (d->features & VIRTIO_BLK_F_SEG_MAX)) ||
        (length < 12 && (d->features & VIRTIO_BLK_F_SIZE_MAX)))
        return -EINVAL;

    for (unsigned i = 0; i < 8; ++i) {
        uint8_t generation = d->common[COMMON_GENERATION];
        uint64_t capacity = config32(d, 0);
        capacity |= (uint64_t)config32(d, 4) << 32;
        uint32_t size = (d->features & VIRTIO_BLK_F_BLK_SIZE) ?
            config32(d, 20) : VIRTIO_BLK_SECTOR_SIZE;
        uint32_t size_max = (d->features & VIRTIO_BLK_F_SIZE_MAX) ? config32(d, 8) : 0;
        uint32_t seg_max = (d->features & VIRTIO_BLK_F_SEG_MAX) ? config32(d, 12) : 1;
        if (generation != d->common[COMMON_GENERATION]) continue;
        if (size < VIRTIO_BLK_SECTOR_SIZE || size > VIRTIO_BLK_MAX_BLOCK_SIZE ||
            (size & (size - 1u)) || !seg_max)
            return -EOPNOTSUPP;
        uint32_t sectors = size / VIRTIO_BLK_SECTOR_SIZE;
        if (!capacity || capacity > UINT64_MAX / VIRTIO_BLK_SECTOR_SIZE || capacity % sectors)
            return -EINVAL;
        uint32_t limit = size_max && size_max < VIRTIO_BLK_MAX_BYTES ?
            size_max : VIRTIO_BLK_MAX_BYTES;
        if (limit < size) return -EOPNOTSUPP;
        *out = (struct blk_geometry){capacity / sectors, size, limit / size, generation};
        return 0;
    }
    return -EBUSY;
}

int virtio_blk_configure(struct virtio_blk *disk)
{
    struct blk_geometry geo;
    int rc = read_geometry(&disk->transport, &geo);
    if (rc) return rc;
    struct block_device_info *info = &disk->registration.info;
    info->blocks = geo.blocks;
    info->block_size = geo.block_size;
    info->max_blocks = geo.max_blocks;
    info->flags = (disk->transport.features & VIRTIO_BLK_F_RO) ? BLOCK_DEVICE_READ_ONLY : 0;
    /* No CONFIG_WCE negotiation: without FLUSH the device is write-through. */
    if (!(disk->transport.features & VIRTIO_BLK_F_FLUSH))
        info->flags |= BLOCK_DEVICE_WRITE_THROUGH;
    disk->config_generation = geo.generation;
    return 0;
}

static int stop_locked(struct virtio_blk *disk)
{
    disk->online = false;
    int rc = disk->transport.common ? virtio_pci_reset(&disk->transport) : 0;
    if (!rc) disk->pending = false;
    return rc;
}

int virtio_blk_stop(void *context)
{
    struct virtio_blk *disk = context;
    kernel_spin_lock(&disk->lock);
    int rc = stop_locked(disk);
    kernel_spin_unlock(&disk->lock);
    return rc;
}

/* A lost completion may have modified the disk. Do not retry it or report a
 * later successful flush on a freshly reset queue with unknown write state. */
static int fault(struct virtio_blk *disk, int error)
{
    int rc = stop_locked(disk);
    const struct pci_device *pci = &disk->transport.pci;
    console_warn("[virtio-blk] pci=%02x:%02x.%u offline: I/O=%d reset=%d\n",
                 pci->bus, pci->slot, pci->function, error, rc);
    return error;
}

static bool device_running(const struct virtio_blk *disk)
{
    uint8_t status = disk->transport.common[COMMON_STATUS];
    return (status & (STATUS_DRIVER_OK | STATUS_NEEDS_RESET | STATUS_FAILED)) == STATUS_DRIVER_OK;
}

static int check_config(struct virtio_blk *disk)
{
    if (!device_running(disk)) return fault(disk, -EIO);
    if (disk->transport.common[COMMON_GENERATION] == disk->config_generation) return 0;
    struct blk_geometry geo;
    const struct block_device_info *info = &disk->registration.info;
    if (read_geometry(&disk->transport, &geo) || geo.blocks != info->blocks ||
        geo.block_size != info->block_size || geo.max_blocks != info->max_blocks)
        return fault(disk, -EIO);
    disk->config_generation = geo.generation;
    return 0;
}

static int finish(struct virtio_blk *disk, void *destination, bool read, bool asynchronous)
{
    /* Retries may migrate CPUs, so only delivered ticks age a pending read.
     * The local work counter can advance with IRQs off, but its PIT fallback
     * can step backwards. Both polling modes also have finite spin budgets. */
    uint64_t poll_start = asynchronous ? 0 : time_work_counter_us();
    uint32_t limit = asynchronous ? 64u : BLK_TIMEOUT_SPINS;
    uint32_t bytes = read ? disk->pending_count * disk->registration.info.block_size : 0;
    for (uint32_t i = 0; i < limit; ++i) {
        void *cookie;
        uint32_t length;
        int rc = virtio_queue_pop(&disk->queue, &cookie, &length);
        if (rc < 0) return fault(disk, -EIO);
        if (rc) {
            if (cookie != disk || !length || length > bytes + 1u)
                return fault(disk, -EIO);
            disk->pending = false;
            rc = check_config(disk);
            if (rc) return rc;
            uint8_t status = *((volatile uint8_t *)disk->dma.cpu + sizeof(struct blk_header));
            if (status == BLK_IOERR) return -EIO;
            if (status == BLK_UNSUPP) return -EOPNOTSUPP;
            if (status != BLK_OK || length != bytes + 1u) return fault(disk, -EIO);
            if (destination && bytes)
                __builtin_memcpy(destination, (const uint8_t *)disk->dma.cpu + VIRTIO_BLK_DATA_OFFSET, bytes);
            return 0;
        }
        if ((i & 255u) == 0) {
            if (!device_running(disk)) return fault(disk, -EIO);
            if (!asynchronous) {
                uint64_t now = time_work_counter_us();
                if (now >= poll_start && now - poll_start >= BLK_TIMEOUT_US) break;
            }
        }
        __asm__ volatile("pause");
    }
    /* The common syscall wait also polls on the next tick if an IRQ is lost. */
    if (asynchronous && time_ticks() - disk->pending_at_tick < BLK_TIMEOUT_SECONDS * RELIEFNT_TICK_HZ)
        return -EAGAIN;
    return fault(disk, -ETIMEDOUT);
}

static int transfer(struct virtio_blk *disk, uint32_t type, uint64_t first,
                    uint32_t count, void *buffer)
{
    int rc;
    kernel_spin_lock(&disk->lock);
    if (!disk->online) { rc = -EIO; goto out; }
    const struct block_device_info *info = &disk->registration.info;
    if (type == BLK_OUT && (info->flags & BLOCK_DEVICE_READ_ONLY)) { rc = -EROFS; goto out; }
    if (type != BLK_FLUSH && (!buffer || !count || count > info->max_blocks ||
        first >= info->blocks || count > info->blocks - first)) { rc = -EINVAL; goto out; }
    rc = check_config(disk);
    if (rc) goto out;

    uint32_t pid = type == BLK_IN ? sched_current_pid() : 0;
    bool asynchronous = pid && storage_async_can_yield();
    if (disk->pending) {
        /* Only reads can have unwound here. A cancelled/mismatched read is
         * drained into owned DMA, then discarded without touching old memory. */
        bool same = pid && disk->pending_pid == pid &&
                    disk->pending_first == first && disk->pending_count == count;
        rc = finish(disk, same ? buffer : NULL, true, asynchronous);
        if (same || rc == -EAGAIN || !disk->online) goto out;
    }
    if (type == BLK_FLUSH && ((info->flags & BLOCK_DEVICE_READ_ONLY) ||
        !(disk->transport.features & VIRTIO_BLK_F_FLUSH))) { rc = 0; goto out; }

    uint32_t bytes = count * info->block_size;
    struct blk_header *header = disk->dma.cpu;
    uint8_t *status = (uint8_t *)disk->dma.cpu + sizeof(*header);
    uint8_t *data = (uint8_t *)disk->dma.cpu + VIRTIO_BLK_DATA_OFFSET;
    *header = (struct blk_header){.type = type,
        .sector = first * (info->block_size / VIRTIO_BLK_SECTOR_SIZE)};
    *status = 0xff;
    if (type == BLK_OUT) __builtin_memcpy(data, buffer, bytes);
    uint64_t phys = disk->dma.physical;
    struct virtio_segment chain[3] = {{phys, sizeof(*header), false}};
    unsigned n = 1;
    if (bytes) chain[n++] = (struct virtio_segment){phys + VIRTIO_BLK_DATA_OFFSET, bytes, type == BLK_IN};
    chain[n++] = (struct virtio_segment){phys + sizeof(*header), 1, true};
    rc = virtio_queue_submit(&disk->queue, chain, n, disk);
    if (rc) { rc = fault(disk, -EIO); goto out; }
    disk->pending = true;
    disk->pending_at_tick = asynchronous ? time_ticks() : 0;
    disk->pending_pid = pid;
    disk->pending_first = first;
    disk->pending_count = count;
    rc = finish(disk, type == BLK_IN ? buffer : NULL, type == BLK_IN, asynchronous);
out:
    kernel_spin_unlock(&disk->lock);
    return rc;
}

static int read_blocks(void *disk, uint64_t first, uint32_t count, void *buffer)
{
    return transfer(disk, BLK_IN, first, count, buffer);
}

static int write_blocks(void *disk, uint64_t first, uint32_t count, const void *buffer)
{
    return transfer(disk, BLK_OUT, first, count, (void *)buffer);
}

static int flush(void *disk)
{
    return transfer(disk, BLK_FLUSH, 0, 0, NULL);
}

static void cancel(void *context, uint32_t pid)
{
    struct virtio_blk *disk = context;
    kernel_spin_lock(&disk->lock);
    if (disk->pending && disk->pending_pid == pid) disk->pending_pid = 0;
    kernel_spin_unlock(&disk->lock);
}

const struct block_device_operations virtio_blk_operations = {
    .read = read_blocks, .write = write_blocks, .flush = flush, .cancel = cancel,
};

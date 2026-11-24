/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <linux/errno.h>
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/module.h>
#include <reliefnt/storage_wait.h>

/* The queue consumer runs only in the I/O owner, not the IRQ. This callback
 * never takes the execution writer or any driver lock needed by reset. */
static void interrupt(void *context)
{
    (void)context;
    storage_completion_notify();
}

/* Consume DMA only after the reset handshake (including IRQ drain). */
static void release_hardware(struct virtio_blk *disk)
{
    (void)virtio_pci_queue_destroy(&disk->transport, &disk->queue);
    (void)dma_buffer_free(&disk->dma);
    virtio_pci_close(&disk->transport);
}

static void release(void *context)
{
    struct virtio_blk *disk = context;
    release_hardware(disk);
    kernel_free(disk);
}

static int probe(const struct pci_device *pci)
{
    struct virtio_blk *disk = kernel_malloc(sizeof(*disk));
    if (!disk) return -ENOMEM;
    __builtin_memset(disk, 0, sizeof(*disk));
    /* Register cleanup before DMA publication, including partially initialized
     * controllers. A failed device STOP must also keep unload from freeing code. */
    int rc = module_add_resource(THIS_MODULE, virtio_blk_stop, release, disk);
    if (rc) { kernel_free(disk); return rc; }

    rc = virtio_pci_open(&disk->transport, pci, VIRTIO_BLK_FEATURES);
    if (rc) goto failed;
    rc = virtio_blk_configure(disk);
    if (rc) goto failed;
    struct block_device_info *info = &disk->registration.info;
    rc = dma_buffer_alloc(&disk->dma, (size_t)info->max_blocks * info->block_size +
                         VIRTIO_BLK_DATA_OFFSET, UINT64_MAX);
    if (rc) goto failed;

    virtio_pci_interrupt_prepare(&disk->transport, interrupt, disk);
    rc = virtio_pci_queue_create(&disk->transport, &disk->queue, 0, 8);
    if (rc) goto failed;
    if (disk->queue.size < 3) { rc = -EOPNOTSUPP; goto failed; }
    rc = virtio_pci_ready(&disk->transport);
    if (rc) goto failed;

    info->device = block_device_allocate_number(false);
    if (!info->device) { rc = -ENOSPC; goto failed; }
    info->transport = BLOCK_TRANSPORT_VIRTIO;
    info->bus = pci->bus;
    info->slot = pci->slot;
    info->function = pci->function;
    __builtin_memcpy(info->model, "VirtIO Block Device", sizeof("VirtIO Block Device"));
    disk->registration.operations = &virtio_blk_operations;
    disk->registration.context = disk;
    disk->registration.owner = THIS_MODULE;
    disk->online = true;
    rc = register_block_device(&disk->registration);
    if (rc) goto failed;

    console_info("[virtio-blk] pci=%02x:%02x.%u blocks=%llu logical=%u max=%u ro=%u flush=%u irq=%s\n",
                 pci->bus, pci->slot, pci->function, (unsigned long long)info->blocks,
                 info->block_size, info->max_blocks, !!(info->flags & BLOCK_DEVICE_READ_ONLY),
                 !!(disk->transport.features & VIRTIO_BLK_F_FLUSH),
                 disk->transport.irq_mode == 1 ? "msix" : disk->transport.irq_mode == 2 ? "msi" : "poll");
    return 0;
failed:
    if (!virtio_blk_stop(disk)) {
        release_hardware(disk);
#ifndef MODULE
        kernel_free(disk);
#endif
    } else {
        console_error("[virtio-blk] pci=%02x:%02x.%u did not reset; retaining DMA resources\n",
                      pci->bus, pci->slot, pci->function);
    }
    return rc;
}

/* Match both PCI IDs, but use only the modern capabilities on a transitional
 * function. Legacy-only devices never reach queue or DMA initialization. */
static int virtio_blk_init(void)
{
    int first_error = 0;
    bool found = false;
    for (uint32_t index = 0;; ++index) {
        struct reliefos_driver_pci_device entry;
        int rc = pci_enumerate(index, &entry);
        if (rc == -ENODEV) break;
        if (rc) return rc;
        if (entry.vendor_id != VIRTIO_BLK_VENDOR ||
            (entry.device_id != VIRTIO_BLK_DEVICE && entry.device_id != VIRTIO_BLK_TRANSITIONAL))
            continue;

        struct pci_device pci;
        if (pci_read_device(entry.bus, entry.slot, entry.function, &pci) ||
            pci.vendor_id != entry.vendor_id || pci.device_id != entry.device_id)
            continue;
        rc = probe(&pci);
        if (!rc) found = true;
        else if (rc == -ENODEV && pci.device_id == VIRTIO_BLK_TRANSITIONAL) {
            /* Lack of modern capabilities must not block an unrelated root
             * device when the boot image loads this module unconditionally. */
            console_debug("[virtio-blk] pci=%02x:%02x.%u has no usable modern interface\n",
                          pci.bus, pci.slot, pci.function);
        } else {
            if (!first_error) first_error = rc;
            console_warn("[virtio-blk] probe pci=%02x:%02x.%u failed: %d\n",
                         pci.bus, pci.slot, pci.function, rc);
        }
    }
    return found ? 0 : first_error;
}

static void virtio_blk_exit(void) { } /* Registry and hardware drain via the ledger. */
block_initcall(virtio_blk_init);
module_exit(virtio_blk_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT modern VirtIO PCI block transport");
MODULE_ALIAS("pci:v00001AF4d00001042sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001AF4d00001001sv*sd*bc*sc*i*");

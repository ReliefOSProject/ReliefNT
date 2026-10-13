/* Sector completion precedes write return. fsync additionally drains the
 * device's volatile cache and propagates errors before publishing account data. */
static int storage_flush_volume_device(const struct storage_volume *volume)
{
    if (!volume) return -19;
    if (volume->kind == STORAGE_VOLUME_RAM || volume->ide_atapi || volume->filesystem == STORAGE_FILESYSTEM_ISO9660) return 0;
    if (volume->transport == STORAGE_TRANSPORT_IDE_PIO) {
        struct ide_device_info device;
        storage_volume_ide_device(volume, &device);
        return ide_flush_cache(&device);
    }
    if (volume->transport == STORAGE_TRANSPORT_NVME)
        return nvme_flush_cache(volume->nvme, volume->nvme_nsid);
    if (volume->transport == STORAGE_TRANSPORT_AHCI)
        return ahci_flush_cache(volume->hba_port);
    return -95;
}

static int storage_flush_volume(const struct storage_volume *volume)
{
    if (!volume || !volume->ready) return -19;
    return storage_flush_volume_device(volume);
}

int storage_ext4_device_flush(const struct storage_volume *volume)
{
    /* Mount-time recovery runs before volume->ready is published. */
    if (volume && volume->kind == STORAGE_VOLUME_RAM) return 0;
    kernel_spin_lock(&storage_transport_lock);
    int ret = storage_flush_volume_device(volume);
    kernel_spin_unlock(&storage_transport_lock);
    return ret;
}

int storage_sync_volume(uint32_t volume_id)
{
    if (volume_id >= STORAGE_MAX_VOLUMES) return -19;
    struct storage_volume *volume = &g_volumes[volume_id];
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    bool saved_async = storage_io_async_context;
    storage_io_async_context = false;
    /* Ext-family volumes publish cached metadata first; cache write-back
     * does its own device I/O and must precede (and never run under) the
     * transport spinlock below. */
    int ret = 0;
    if (volume->ready &&
        (volume->filesystem == STORAGE_FILESYSTEM_EXT4 ||
         volume->filesystem == STORAGE_FILESYSTEM_EXT2) &&
        storage_ext4_cache_flush) {
        ret = storage_ext4_journal_commit(volume, true);
        if (!ret) ret = storage_ext4_journal_checkpoint(volume);
        if (!ret) ret = storage_ext4_cache_flush(volume);
    }
    int transport_ret;
    if (volume->kind == STORAGE_VOLUME_RAM) transport_ret = storage_flush_volume(volume);
    else {
        kernel_spin_lock(&storage_transport_lock);
        transport_ret = storage_flush_volume(volume);
        kernel_spin_unlock(&storage_transport_lock);
    }
    if (ret == 0) ret = transport_ret;
    storage_io_async_context = saved_async;
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_sync_all(void)
{
    int result = 0;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    for (uint32_t i = 0; i < STORAGE_MAX_VOLUMES; ++i) {
        if (!g_volumes[i].ready) continue;
        int ret = storage_sync_volume(i);
        if (ret < 0 && !result) result = ret;
    }
    kernel_execution_unlock_irqrestore(flags);
    return result;
}

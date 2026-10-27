#include "internal.h"
#include <reliefnt/mount.h>
#include <linux/errno.h>

static uint32_t storage_next_data_mount_volume(void);

static int storage_filesystem_from_name(const char *filesystem, uint32_t *out_filesystem);

/**
 * @brief Report mounted filesystems conflicting with an O_EXCL block open.
 * Caller holds the execution lock. Whole disks conflict with every mounted
 * partition; a partition conflicts only with its own mounted dev_t identity.
 */
int storage_disk_block_busy(uint32_t disk_id, int32_t partition_index)
{
    if(disk_id&STORAGE_BLOCK_OPTICAL_BIT) {
        if(partition_index!=-1)return -22;
        struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);if(rc)return rc;
        int busy=0;
        for(uint32_t i=0;i<STORAGE_MAX_VOLUMES;++i)
            if(g_volumes[i].ready && g_volumes[i].device_ref.device==ref.device && g_volumes[i].vfs_mounts){busy=1;break;}
        block_device_put(&ref);return busy;
    }

    if (disk_id >= g_install_disk_count || !g_install_disks[disk_id].present ||
        partition_index < -1 || (partition_index >= 0 &&
        (uint32_t)partition_index >= RELIEFOS_DISK_MAX_PARTITIONS)) return -22;
    uint64_t device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(disk_id, partition_index));
    for (uint32_t i = 0; i < STORAGE_MAX_VOLUMES; ++i) {
        const struct storage_volume *volume = &g_volumes[i];
        if (!volume->ready || volume->transport==STORAGE_TRANSPORT_MEMORY || volume->tmpfs ||
            volume->source_disk_id != disk_id) continue;
        /* storage_volume_device_number assigns anonymous major-zero IDs
         * 1..STORAGE_MAX_VOLUMES to optical/pseudo mounts. These do not own
         * an install disk, regardless of their default source_disk_id. */
        uint64_t mounted_device = storage_volume_device_number(volume);
        if (mounted_device <= STORAGE_MAX_VOLUMES) continue;
        if (partition_index < 0 || mounted_device == device) return 1;
    }
    return 0;
}


/**
 * @brief Checks whether a runtime data mount belongs to a block disk.
 * @param disk_id Block-disk identifier to test.
 * @return Nonzero when a dynamic data mount references @p disk_id.
 */
int storage_disk_has_data_mount(uint32_t disk_id)
{
    for (uint32_t volume_id = STORAGE_VOLUME_TARGET_ROOT;
         volume_id < STORAGE_MAX_VOLUMES; ++volume_id) {
        const struct storage_volume *volume = &g_volumes[volume_id];
        if (volume->ready && volume->data_partition_mount &&
            volume->source_disk_id == disk_id) {
            return 1;
        }
    }
    return 0;
}

int storage_disk_partition_volume_id(uint32_t disk_id, uint32_t partition_index,
                                     uint32_t *out_volume_id)
{
    if (!out_volume_id || partition_index >= RELIEFOS_DISK_MAX_PARTITIONS) {
        return -22;
    }
    for (uint32_t volume_id = STORAGE_VOLUME_TARGET_ROOT;
         volume_id < STORAGE_MAX_VOLUMES; ++volume_id) {
        const struct storage_volume *volume = &g_volumes[volume_id];
        if (volume->ready && volume->data_partition_mount &&
            volume->source_disk_id == disk_id &&
            volume->source_partition_index == partition_index) {
            *out_volume_id = volume_id;
            return 0;
        }
    }
    return -2;
}

/** @brief Describe the selected partition's actual live attachment for users.
 * @param disk_id Disk inventory identifier, never interpreted as a host disk.
 * @param partition_index Zero-based partition in the existing volume registry.
 * @param out_path Required diagnostic output, empty on absent/format failure.
 * @param capacity Bytes including NUL; names that do not fit are not truncated.
 * @return Zero, ENOENT before namespace publication/after detach, or negative
 * argument/format errno. Caller holds execution ownership. No lookup, I/O,
 * allocation or ownership transfer; this output never routes backend work.
 */
int storage_disk_partition_mount_path(uint32_t disk_id, uint32_t partition_index,
                                      char *out_path, uint32_t capacity)
{
    uint32_t volume_id;
    if (!out_path || capacity == 0) {
        return -22;
    }
    if (storage_disk_partition_volume_id(disk_id, partition_index, &volume_id) < 0) {
        out_path[0] = 0;
        return -2;
    }
    out_path[0]=0;
    struct vfs_namespace *ns=vfs_system_namespace();if(!ns)return -2;
    return vfs_namespace_context_mountpoint(ns,&g_volumes[volume_id],out_path,capacity);
}

static uint32_t storage_next_data_mount_volume(void)
{
    for (uint32_t volume_id = STORAGE_VOLUME_DYNAMIC_FIRST;
         volume_id < STORAGE_MAX_VOLUMES; ++volume_id) {
        if (!g_volumes[volume_id].ready) {
            return volume_id;
        }
    }
    return STORAGE_MAX_VOLUMES;
}

static int storage_filesystem_from_name(const char *filesystem, uint32_t *out_filesystem)
{
    if (!out_filesystem) {
        return -22;
    }
    if (!filesystem || !filesystem[0] || storage_text_eq_ci(filesystem, "auto")) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_UNKNOWN;
        return 0;
    }
    if (storage_text_eq_ci(filesystem, "fat") || storage_text_eq_ci(filesystem, "vfat") ||
        storage_text_eq_ci(filesystem, "fat32")) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_FAT32;
        return 0;
    }
    if (storage_text_eq_ci(filesystem, "ext2")) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_EXT2;
        return 0;
    }
    if (storage_text_eq_ci(filesystem, "ext4")) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_EXT4;
        return 0;
    }
    if (storage_text_eq_ci(filesystem, "exfat")) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_EXFAT;
        return 0;
    }
    return -95;
}

/** @brief Find an existing physical superblock by its actual standard dev_t.
 * @param disk Registered disk identifier, validated by caller.
 * @param partition Valid partition index.
 * @param requested Requested public filesystem kind, UNKNOWN for detection.
 * @param flags New attachment request; shared readonly must agree.
 * @param slot Optional matched backend index output.
 * @return Zero on reuse, ENOENT when absent, EINVAL on type mismatch or EBUSY
 * when readonly would change. Another attachment does not reconfigure existing
 * shared sync policy. Pure execution-owned query, no allocation, disk I/O,
 * module callback or ownership change; caller promptly retains VFS membership.
 */
static int storage_existing_block_partition(uint32_t disk,uint32_t partition,uint32_t requested,
                                               uint64_t flags,uint32_t *slot)
{
    uint64_t device=storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(disk,partition));
    for(uint32_t i=0;i<STORAGE_MAX_VOLUMES;++i) {
        const struct storage_volume *volume=&g_volumes[i];
        if(!volume->ready || volume->transport==STORAGE_TRANSPORT_MEMORY || volume->tmpfs || volume->source_disk_id!=disk ||
           storage_volume_device_number(volume)!=device)continue;
        uint32_t actual=volume->filesystem==STORAGE_FILESYSTEM_FAT32 ? RELIEFOS_DISK_FILESYSTEM_FAT32 :
            volume->filesystem==STORAGE_FILESYSTEM_EXFAT ? RELIEFOS_DISK_FILESYSTEM_EXFAT :
            volume->filesystem==STORAGE_FILESYSTEM_EXT2 ? RELIEFOS_DISK_FILESYSTEM_EXT2 :
            volume->filesystem==STORAGE_FILESYSTEM_EXT4 ? RELIEFOS_DISK_FILESYSTEM_EXT4 : RELIEFOS_DISK_FILESYSTEM_UNKNOWN;
        if(!actual)return -EOPNOTSUPP;
        if(requested && requested!=actual)return -EINVAL;
        bool readonly=(volume->mount_flags&MS_RDONLY) || volume->read_only_reason;
        if(!!(flags&MS_RDONLY)!=readonly)return -EBUSY;
        if(slot)*slot=i;return 0;
    }
    return -ENOENT;
}

/** @brief Reuse a physical filesystem instance or prepare one new GPT backend.
 * @param disk_id Standard registered disk identifier.
 * @param partition_index GPT partition index.
 * @param filesystem_name Kernel type string, NULL/auto for native detection.
 * @param flags Supported MS policy bits without operation selectors.
 * @param out_volume_id Optional matched/new backend index output.
 * @param created Optional output, false on failure/reuse, true on new success.
 * @return Zero or negative input/type/readonly/device/I/O/capacity errno. Reuse
 * is a pure dev_t/type/readonly query; new backend I/O uses the execution-owned
 * synchronous transaction. No target text is published. Caller owns rollback
 * only when created is true, and must retain a VFS membership before yielding.
 */
static int storage_open_block_partition_owned(uint32_t disk_id, uint32_t partition_index,
                                  const char *filesystem_name,
                                  uint64_t flags, uint32_t *out_volume_id,bool *created)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    struct gpt_entry entry;
    struct storage_volume *volume;
    struct storage_volume *old_volume;
    uint64_t sector_count;
    uint32_t filesystem;
    uint32_t volume_id;
    int ret;
    uint32_t requested_filesystem;

    if(created)*created=false;
    if(out_volume_id)*out_volume_id=0;

    console_debug("[storage] mount enter disk=%u part=%u fs=%s flags=%llu\n",
                   disk_id, partition_index,
                   filesystem_name ? filesystem_name : "auto",
                   (unsigned long long)flags);
    const uint64_t supported=MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_DIRSYNC|
        MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME|MS_LAZYTIME;
    if ((flags & ~supported) ||
        storage_filesystem_from_name(filesystem_name, &requested_filesystem) < 0) {
        console_error("[storage] mount invalid arguments flags=%llu fs_name=%s\n",
                       (unsigned long long)flags,
                       filesystem_name ? filesystem_name : "(null)");
        return -22;
    }
    if(disk_id>=g_install_disk_count || !g_install_disks[disk_id].present)return -ENODEV;
    if(partition_index>=RELIEFOS_DISK_MAX_PARTITIONS)return -EINVAL;
    ret=storage_existing_block_partition(disk_id,partition_index,requested_filesystem,flags,out_volume_id);
    if(ret!=-ENOENT)return ret;
    ret = storage_acquire_task_io();
    if (ret < 0) {
        console_error("[storage] mount acquire io failed ret=%d\n", ret);
        return ret;
    }
    ret = disk_manage_prepare(disk_id, &disk, &sector_count);
    if (ret < 0) {
        console_error("[storage] mount disk prepare failed disk=%u ret=%d\n", disk_id, ret);
        return ret;
    }
    ret=storage_disk_block_busy(disk_id,(int32_t)partition_index);
    if(ret)return ret<0 ? ret : -16;
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0 || partition_index >= table.primary.partition_entry_count) {
        console_error("[storage] mount gpt load failed disk=%u part=%u ret=%d\n",
                       disk_id, partition_index, ret < 0 ? ret : -22);
        return ret < 0 ? ret : -22;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        console_error("[storage] mount gpt entries invalid disk=%u part=%u ret=%d\n",
                       disk_id, partition_index, ret);
        return ret;
    }
    {
        const struct gpt_entry *entries = (const struct gpt_entry *)(const void *)storage_cluster_buf;
        if (!disk_gpt_entry_used(&entries[partition_index])) {
            return -2;
        }
        entry = entries[partition_index];
    }
    ret = disk_partition_filesystem(disk, &entry, &filesystem);
    if (ret < 0) {
        console_error("[storage] mount filesystem probe failed disk=%u part=%u ret=%d\n",
                       disk_id, partition_index, ret);
        return ret;
    }
    if (!disk_filesystem_format_supported(filesystem)) {
        console_warn("[storage] mount unsupported filesystem disk=%u part=%u fs=%u\n",
                       disk_id, partition_index, filesystem);
        return -95;
    }
    if (requested_filesystem && requested_filesystem != filesystem) {
        console_warn("[storage] mount filesystem mismatch disk=%u part=%u detected=%u requested=%u\n",
                       disk_id, partition_index, filesystem, requested_filesystem);
        return -22;
    }
    volume_id = storage_next_data_mount_volume();
    if (volume_id == STORAGE_MAX_VOLUMES) {
        return -28;
    }
    if (g_volumes[volume_id].ready) {
        return -16;
    }

    volume = &g_volumes[volume_id];
    old_volume = g_active_volume;
    storage_cache_invalidate();
    storage_memzero(volume, sizeof(*volume));
    volume->volume_id = (uint8_t)volume_id;
    ret=storage_volume_from_install_disk(volume,disk);
    if(ret)return ret;
    volume->mount_flags = flags;
    volume->source_disk_id = disk_id;
    volume->source_partition_index = partition_index;
    if (filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32) {
        volume->esp_start_lba = entry.first_lba;
        volume->esp_sector_count = entry.last_lba - entry.first_lba + 1u;
    } else if (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT) {
        volume->exfat_start_lba = entry.first_lba;
        volume->exfat_sector_count = entry.last_lba - entry.first_lba + 1u;
    } else {
        volume->ext2_start_lba = entry.first_lba;
        volume->ext2_sector_count = entry.last_lba - entry.first_lba + 1u;
    }
    g_active_volume = volume;
    ret = storage_mount_filesystem(volume,filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ? "vfat" :
          filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT ? "exfat" :
          filesystem == RELIEFOS_DISK_FILESYSTEM_EXT2 ? "ext2" : "ext4");
    if (ret == 0 &&
        ((filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 &&
          volume->filesystem != STORAGE_FILESYSTEM_FAT32) ||
         (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT &&
          volume->filesystem != STORAGE_FILESYSTEM_EXFAT) ||
         ((filesystem == RELIEFOS_DISK_FILESYSTEM_EXT2 ||
           filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4) &&
          !storage_filesystem_is_ext(volume)))) {
        ret = -5;
    }
    if (ret == 0) {
        volume->data_partition_mount = 1;
        volume->ready = true;
        console_info("[reliefnt] storage opened data partition disk=%u entry=%u fs=%s\n",
                       disk_id, partition_index,
                       filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ? "fat32" :
                       (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT ? "exfat" :
                        (filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4 ? "ext4" : "ext2")));
        if (out_volume_id) {
            *out_volume_id = volume_id;
        }
        if(created)*created=true;
    } else {
        storage_destroy_volume(volume);
        storage_memzero(volume, sizeof(*volume));
    }
    g_active_volume = old_volume;
    storage_cache_invalidate();
    return ret;
}

/** @brief Retain the exact block/code identity for a complete mount transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @param partition_index GPT entry index or whole-device selector as supported.
 * @param filesystem_name Borrowed optional requested native type.
 * @param flags Existing validated mount policy.
 * @param out_volume_id Worker output identifying the initialized volume on success.
 * @param created Worker output identifying fresh ownership on success.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_open_block_partition(uint32_t disk_id, uint32_t partition_index,
                                  const char *filesystem_name,
                                  uint64_t flags, uint32_t *out_volume_id,bool *created)
{
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc)rc=storage_open_block_partition_owned(disk_id,partition_index,filesystem_name,flags,out_volume_id,created);
    block_device_put(&ref);return rc;
}

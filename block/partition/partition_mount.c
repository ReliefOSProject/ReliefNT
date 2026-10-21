#include "internal.h"

static uint32_t storage_next_data_mount_volume(void);

static int storage_mount_path_allowed(const char *path);

static int storage_filesystem_from_name(const char *filesystem, uint32_t *out_filesystem);

static uint32_t storage_mount_volume_for_target(const char *target);


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

/**
 * @brief Checks whether installer-reserved mount slots are occupied.
 * @return Nonzero when /target or /target/boot cannot be reused by the installer.
 */
int storage_installer_mounts_busy(void)
{
    for (uint32_t volume_id = STORAGE_VOLUME_TARGET_ROOT;
         volume_id <= STORAGE_VOLUME_BOOT; ++volume_id) {
        const struct storage_volume *volume = &g_volumes[volume_id];
        if (volume->ready) {
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
    storage_copy_text(out_path, capacity, g_volumes[volume_id].mount_path);
    return 0;
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

static int storage_mount_path_allowed(const char *path)
{
    uint32_t length;
    if (!path || path[0] != '/') {
        return -22;
    }
    length = storage_strlen(path);
    if (length < 2u || length >= RELIEFOS_FS_PATH_LEN || path[length] != 0) {
        return -22;
    }
    /* Never let a user mount over a kernel-managed namespace. /target and
     * /target/boot are the installer ISO's explicit standard mount points. */
    if (storage_text_eq_ci(path, "/boot") || storage_text_eq_ci(path, "/dev")) {
        return -16;
    }
    if ((storage_text_eq_ci(path, "/target") ||
         storage_text_eq_ci(path, "/target/boot")) && !storage_installer_root_active()) {
        return -16;
    }
    for (uint32_t i = STORAGE_VOLUME_TARGET_ROOT; i < STORAGE_MAX_VOLUMES; ++i) {
        if (g_volumes[i].ready && storage_text_eq_ci(g_volumes[i].mount_path, path)) {
            return -16;
        }
    }
    return 0;
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

static uint32_t storage_mount_volume_for_target(const char *target)
{
    if (target && storage_text_eq_ci(target, "/target")) {
        return STORAGE_VOLUME_TARGET_ROOT;
    }
    if (target && storage_text_eq_ci(target, "/target/boot")) {
        return STORAGE_VOLUME_BOOT;
    }
    return storage_next_data_mount_volume();
}

/** Mount a GPT partition through the standard block-device and mount ABI. */
int storage_mount_block_partition(uint32_t disk_id, uint32_t partition_index,
                                  const char *target, const char *filesystem_name,
                                  uint64_t flags, uint32_t *out_volume_id)
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
    char mounted_path[RELIEFOS_FS_PATH_LEN];
    uint32_t requested_filesystem;

    console_debug("[storage] mount enter disk=%u part=%u target=%s fs=%s flags=%llu\n",
                   disk_id, partition_index, target ? target : "(auto)",
                   filesystem_name ? filesystem_name : "auto",
                   (unsigned long long)flags);
    if ((flags & ~(uint64_t)(MS_NOSUID | MS_NOEXEC)) ||
        storage_filesystem_from_name(filesystem_name, &requested_filesystem) < 0) {
        console_error("[storage] mount invalid arguments flags=%llu fs_name=%s\n",
                       (unsigned long long)flags,
                       filesystem_name ? filesystem_name : "(null)");
        return -22;
    }
    if (target && target[0]) {
        ret = storage_mount_path_allowed(target);
        if (ret < 0) {
            console_info("[storage] mount path not allowed target=%s ret=%d\n",
                           target, ret);
            return ret;
        }
    }
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
    ret = disk_partition_mutable(disk);
    if (ret < 0) {
        console_info("[storage] mount disk immutable disk=%u ret=%d\n", disk_id, ret);
        return ret;
    }
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
    if (storage_disk_partition_mount_path(disk_id, partition_index,
                                          mounted_path, sizeof(mounted_path)) == 0) {
        if (target && !storage_text_eq_ci(target, mounted_path)) {
            return -16;
        }
        uint32_t mounted_id;
        ret = storage_disk_partition_volume_id(disk_id, partition_index, &mounted_id);
        if (ret < 0) return ret;
        if (g_volumes[mounted_id].mount_flags != flags) return -16;
        if (out_volume_id) {
            (void)storage_disk_partition_volume_id(disk_id, partition_index,
                                                   out_volume_id);
        }
        return 0;
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
    volume_id = storage_mount_volume_for_target(target);
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
    storage_volume_from_install_disk(volume, disk);
    volume->mount_flags = flags;
    volume->source_disk_id = disk_id;
    volume->source_partition_index = partition_index;
    if (!target || !target[0]) {
        if (storage_set_data_mount_path(volume, disk_id, partition_index) < 0) {
            storage_memzero(volume, sizeof(*volume));
            return -22;
        }
    } else {
        storage_copy_text(volume->mount_path, sizeof(volume->mount_path), target);
    }
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
    ret = filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ? fat32_mount() :
          (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT ? exfat_mount() : ext2_mount());
    if (ret == 0 &&
        ((filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 &&
          volume->filesystem != STORAGE_FILESYSTEM_FAT32) ||
         (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT &&
          volume->filesystem != STORAGE_FILESYSTEM_EXFAT) ||
         ((filesystem == RELIEFOS_DISK_FILESYSTEM_EXT2 ||
           filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4) &&
          !storage_ext4_is_ext_family(volume)))) {
        ret = -5;
    }
    if (ret == 0) {
        volume->data_partition_mount = 1;
        volume->ready = true;
        console_info("[reliefnt] storage mounted data partition disk=%u entry=%u path=%s fs=%s\n",
                       disk_id, partition_index, volume->mount_path,
                       filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ? "fat32" :
                       (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT ? "exfat" :
                        (filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4 ? "ext4" : "ext2")));
        if (out_volume_id) {
            *out_volume_id = volume_id;
        }
    } else {
        storage_memzero(volume, sizeof(*volume));
    }
    g_active_volume = old_volume;
    storage_cache_invalidate();
    return ret;
}

/** Legacy ABI adapter retained until all callers have moved to mount(2). */
int storage_disk_mount_partition(struct reliefos_disk_partition_mount *request)
{
    uint32_t volume_id;
    int ret;
    if (!request) {
        return -22;
    }
    ret = storage_mount_block_partition(request->disk_id, request->partition_index,
                                        request->mount_path[0] ? request->mount_path : NULL,
                                        NULL, 0, &volume_id);
    if (ret == 0 && volume_id < STORAGE_MAX_VOLUMES) {
        storage_copy_text(request->mount_path, sizeof(request->mount_path),
                          g_volumes[volume_id].mount_path);
    }
    return ret;
}

int storage_mount_path_volume_id(const char *target, uint32_t *out_volume_id)
{
    if (!target || !out_volume_id) {
        return -22;
    }
    for (uint32_t volume_id = STORAGE_VOLUME_TARGET_ROOT;
         volume_id < STORAGE_MAX_VOLUMES; ++volume_id) {
        const struct storage_volume *volume = &g_volumes[volume_id];
        if (volume->ready && (volume->data_partition_mount || volume->tmpfs) &&
            storage_text_eq(volume->mount_path, target)) {
            *out_volume_id = volume_id;
            return 0;
        }
    }
    return -2;
}

int storage_unmount_path(const char *target, uint32_t *out_volume_id)
{
    uint32_t volume_id;
    uint32_t target_length;
    int ret;
    if (!target) {
        return -22;
    }
    ret = storage_mount_path_volume_id(target, &volume_id);
    if (ret < 0) {
        return ret;
    }
    if (storage_inode_volume_busy(volume_id)) return -16;
    target_length = storage_strlen(target);
    for (uint32_t i = STORAGE_VOLUME_TARGET_ROOT; i < STORAGE_MAX_VOLUMES; ++i) {
        const struct storage_volume *volume = &g_volumes[i];
        if (i != volume_id && volume->ready &&
            storage_memcmp(volume->mount_path, target, target_length) == 0 &&
            volume->mount_path[target_length] == '/') {
            return -16;
        }
    }
    ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    ret = storage_sync_volume(volume_id);
    if (ret < 0) return ret;
    if (g_active_volume == &g_volumes[volume_id]) {
        g_active_volume = &g_volumes[STORAGE_VOLUME_ROOT];
    }
    console_info("[reliefnt] storage unmounted data partition disk=%u entry=%u path=%s\n",
                   g_volumes[volume_id].source_disk_id,
                   g_volumes[volume_id].source_partition_index, target);
    tmpfs_destroy(g_volumes[volume_id].tmpfs);
    storage_ext4_journal_close(&g_volumes[volume_id]);
    storage_memzero(&g_volumes[volume_id], sizeof(g_volumes[volume_id]));
    storage_cache_invalidate();
    if (out_volume_id) {
        *out_volume_id = volume_id;
    }
    return 0;
}

/**
 * @brief Unmounts a runtime data partition after the kernel has checked task usage.
 * @param request Disk and GPT-entry selector.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_unmount_partition(const struct reliefos_disk_partition_unmount *request)
{
    uint32_t volume_id;
    int ret;
    if (!request || request->reserved0 != 0 || request->reserved1 != 0) {
        return -22;
    }
    ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    ret = storage_disk_partition_volume_id(request->disk_id, request->partition_index,
                                           &volume_id);
    if (ret < 0) {
        return ret;
    }
    if (volume_id >= STORAGE_MAX_VOLUMES || !g_volumes[volume_id].data_partition_mount) {
        return -2;
    }
    return storage_unmount_path(g_volumes[volume_id].mount_path, NULL);
}

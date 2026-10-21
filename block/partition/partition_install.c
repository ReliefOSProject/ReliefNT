#include "internal.h"



int storage_install_list_disks(struct reliefos_install_disk *disks,
                               uint32_t capacity, uint32_t *out_count)
{
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!out_count) {
        return -22;
    }
    *out_count = g_install_disk_count;
    if (capacity > g_install_disk_count) {
        capacity = g_install_disk_count;
    }
    if (capacity && !disks) {
        return -22;
    }
    for (uint32_t i = 0; i < capacity; ++i) {
        struct install_disk_state *src = &g_install_disks[i];
        if (src->present && src->sector_count == 0) {
            uint64_t sectors = 0;
            if (storage_install_identify(src, &sectors) == 0) {
                src->sector_count = sectors;
            }
        }
        disks[i].id = i;
        disks[i].port = src->transport == STORAGE_TRANSPORT_NVME
                            ? src->nvme_nsid : src->port;
        disks[i].sector_size = SECTOR_SIZE;
        disks[i].flags = 0;
        if (src->boot_root) {
            disks[i].flags |= RELIEFOS_INSTALL_DISK_FLAG_BOOT_ROOT;
        }
        if (src->target_mounted) {
            disks[i].flags |= RELIEFOS_INSTALL_DISK_FLAG_TARGET_MOUNTED;
        }
        disks[i].sector_count = src->sector_count;
        storage_copy_text(disks[i].name, sizeof(disks[i].name),
                          src->transport == STORAGE_TRANSPORT_IDE_PIO
                              ? "IDE/PATA Disk"
                              : (src->transport == STORAGE_TRANSPORT_NVME
                                     ? "NVMe Namespace" : "SATA/AHCI Disk"));
    }
    return 0;
}

int storage_install_format_target(uint32_t disk_id)
{
    uint64_t sector_count;
    uint64_t esp_lba = 0;
    uint64_t esp_sectors = 0;
    uint64_t root_lba = 0;
    uint64_t root_sectors = 0;
    int ret;
    ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (disk_id >= g_install_disk_count || !g_install_disks[disk_id].present) {
        return -2;
    }
    struct install_disk_state *disk = &g_install_disks[disk_id];
    if (disk->target_mounted || storage_disk_has_data_mount(disk_id) ||
        storage_installer_mounts_busy()) {
        return -RELIEFOS_EBUSY;
    }
    ret = storage_prepare_install_disk(disk);
    if (ret < 0) {
        return ret;
    }
    sector_count = disk->sector_count;
    if (sector_count == 0) {
        ret = storage_install_identify(disk, &sector_count);
        if (ret < 0) {
            return ret;
        }
        disk->sector_count = sector_count;
    }
    ret = install_write_gpt(disk, sector_count, &esp_lba, &esp_sectors,
                            &root_lba, &root_sectors);
    if (ret < 0) {
        return ret;
    }
    ret = install_format_fat32(disk, esp_lba, esp_sectors);
    if (ret < 0) {
        return ret;
    }
    ret = install_format_ext4(disk, root_lba, root_sectors);
    if (ret < 0) {
        return ret;
    }
    disk->target_mounted = 0;
    storage_memzero(&g_volumes[STORAGE_VOLUME_TARGET_ROOT],
                    sizeof(g_volumes[STORAGE_VOLUME_TARGET_ROOT]));
    storage_memzero(&g_volumes[STORAGE_VOLUME_BOOT], sizeof(g_volumes[STORAGE_VOLUME_BOOT]));
    if (g_active_volume == &g_volumes[STORAGE_VOLUME_TARGET_ROOT] ||
        g_active_volume == &g_volumes[STORAGE_VOLUME_BOOT]) {
        g_active_volume = &g_volumes[STORAGE_VOLUME_ROOT];
    }
    disk_block_cache_invalidate(disk_id);
    return 0;
}

/**
 * @brief Preserves the historical installer formatting ABI.
 * @param disk_id Installer disk identifier.
 * @return Zero after formatting the ESP plus exFAT root, or a negative error.
 */
int storage_install_format_esp(uint32_t disk_id)
{
    return storage_install_format_target(disk_id);
}

int storage_install_mount_target(uint32_t disk_id)
{
    int ownership_ret = storage_acquire_task_io();
    if (ownership_ret < 0) {
        return ownership_ret;
    }
    if (disk_id >= g_install_disk_count || !g_install_disks[disk_id].present) {
        return -2;
    }
    struct install_disk_state *disk = &g_install_disks[disk_id];
    if (disk->target_mounted) {
        return 0;
    }
    if (storage_disk_has_data_mount(disk_id) || storage_installer_mounts_busy()) {
        return -RELIEFOS_EBUSY;
    }
    struct storage_volume *target = &g_volumes[STORAGE_VOLUME_TARGET_ROOT];
    struct storage_volume *esp = &g_volumes[STORAGE_VOLUME_BOOT];
    struct storage_volume *old = g_active_volume;
    storage_cache_invalidate();
    storage_memzero(target, sizeof(*target));
    target->volume_id = STORAGE_VOLUME_TARGET_ROOT;
    storage_volume_from_install_disk(target, disk);
    storage_copy_text(target->mount_path, sizeof(target->mount_path), "/target");
    int ret = storage_prepare_install_disk(disk);
    if (ret < 0) {
        g_active_volume = old;
        return ret;
    }
    g_active_volume = target;
    ret = gpt_find_esp();
    if (ret == 0 && target->exfat_start_lba) {
        ret = exfat_mount();
        console_info("[reliefnt] installer exFAT target mount returned %d\n", ret);
    }
    if (ret == 0 && target->filesystem != STORAGE_FILESYSTEM_EXFAT && target->ext2_start_lba) {
        ret = ext2_mount();
    }
    if (ret == 0 && (target->filesystem == STORAGE_FILESYSTEM_EXFAT ||
                     storage_ext4_is_ext_family(target))) {
        target->ready = true;
        disk->target_mounted = 1;
        storage_memzero(esp, sizeof(*esp));
        *esp = *target;
        storage_memzero(&esp->ext4, sizeof(esp->ext4));
        esp->read_only_reason = STORAGE_EXT4_READ_ONLY_NONE;
        esp->volume_id = STORAGE_VOLUME_BOOT;
        esp->ready = false;
        storage_copy_text(esp->mount_path, sizeof(esp->mount_path), "/target/boot");
        g_active_volume = esp;
        ret = fat32_mount();
        if (ret < 0) {
            console_error("[reliefnt] installer ESP mount failed ret=%d lba=%llu sectors=%llu\n",
                           ret, (unsigned long long)esp->esp_start_lba,
                           (unsigned long long)esp->esp_sector_count);
        }
        if (ret == 0) esp->ready = true;
    }
    if (ret == 0 && (target->filesystem == STORAGE_FILESYSTEM_EXFAT ||
                     storage_ext4_is_ext_family(target))) {
        console_info("[reliefnt] installer target mounted root=/target %s_lba=%llu esp=/target/boot esp_lba=%llu disk=%u port=%u\n",
                       target->filesystem == STORAGE_FILESYSTEM_EXFAT ? "exfat" :
                       (target->filesystem == STORAGE_FILESYSTEM_EXT4 ? "ext4" : "ext2"),
                       (unsigned long long)(target->filesystem == STORAGE_FILESYSTEM_EXFAT
                                            ? target->exfat_start_lba : target->ext2_start_lba),
                       (unsigned long long)esp->esp_start_lba,
                       disk_id,
                       disk->port);
    } else {
        storage_memzero(target, sizeof(*target));
        storage_memzero(esp, sizeof(*esp));
        disk->target_mounted = 0;
    }
    g_active_volume = old;
    return ret;
}

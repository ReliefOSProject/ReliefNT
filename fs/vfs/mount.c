#include "../../block/core/storage_internal.h"
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/boot_block.h>

/** @brief Retire unpublished discovery instances before a boot-only reset.
 * @return None. BSP boot setup only, before namespace/inode users; no callback
 * lock is held and every native destructor runs before its code pin drops.
 */
static void storage_boot_drop_instances(void)
{
    for(uint32_t i=0;i<STORAGE_MAX_VOLUMES;++i)storage_destroy_volume(&g_volumes[i]);
}

static int storage_path_append_u32(char *path, uint32_t capacity, uint32_t *position,
                                   uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    if (!path || !position || *position >= capacity) {
        return -22;
    }
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) {
        if (*position + 1 >= capacity) {
            return -22;
        }
        path[(*position)++] = digits[--count];
    }
    path[*position] = 0;
    return 0;
}

static int storage_path_append_text(char *path, uint32_t capacity, uint32_t *position,
                                    const char *text)
{
    if (!path || !position || !text) return -22;
    while (*text) {
        if (*position + 1u >= capacity) return -22;
        path[(*position)++] = *text++;
    }
    path[*position] = 0;
    return 0;
}

static int storage_append_sd_suffix(char *out, uint32_t capacity, uint32_t *position,
                                    uint32_t index)
{
    char letters[8];
    uint32_t count = 0;
    do {
        if (count >= sizeof(letters)) return -22;
        letters[count++] = (char)('a' + (index % 26u));
        index = index / 26u;
        if (index) --index;
    } while (index || count == 0);
    while (count) {
        if (*position + 1u >= capacity) return -22;
        out[(*position)++] = letters[--count];
    }
    out[*position] = 0;
    return 0;
}

/** @brief Format a standard name from immutable core disk/optical identity.
 * @param disk_id Reserved disk slot or flagged optical minor.
 * @param partition_index GPT slot, or -1 for a whole device (required for sr).
 * @param out Writable capacity-byte output. @param capacity Nonzero bytes.
 * @return Zero or -ENOENT/-EINVAL. Execution ownership; observes scalar records
 * without allocation, provider callback, hardware access or code reference.
 * Legacy endpoint hints may reuse unused names; USB replug receives a new
 * dynamic name, while old inode dev_t stays unchanged.
 */
int storage_disk_device_name(uint32_t disk_id, int32_t partition_index,
                             char *out, uint32_t capacity)
{
    const struct install_disk_state *disk;
    uint32_t position = 0;
    if(disk_id&STORAGE_BLOCK_OPTICAL_BIT) {
        if(partition_index!=-1 || !out || !capacity)return -2;
        out[0]=0;
        if(storage_path_append_text(out,capacity,&position,"/dev/sr"))return -22;
        return storage_path_append_u32(out,capacity,&position,disk_id&~STORAGE_BLOCK_OPTICAL_BIT);
    }
    if (!out || capacity == 0 || disk_id >= g_install_disk_count ||
        partition_index < -1 || partition_index >= (int32_t)RELIEFOS_DISK_MAX_PARTITIONS ||
        !g_install_disks[disk_id].present) return -2;
    disk = &g_install_disks[disk_id];
    out[0] = 0;
    if(disk->transport==STORAGE_TRANSPORT_MEMORY) {
        if(partition_index!=-1)return -2;
        if(storage_path_append_text(out,capacity,&position,"/dev/ram") ||
           storage_path_append_u32(out,capacity,&position,disk->name_index))return -22;
    } else if (disk->transport == STORAGE_TRANSPORT_NVME) {
        if (storage_path_append_text(out, capacity, &position, "/dev/nvme") < 0 ||
            storage_path_append_u32(out, capacity, &position,
                                    disk->nvme_controller_index) < 0 ||
            storage_path_append_text(out, capacity, &position, "n") < 0 ||
            storage_path_append_u32(out, capacity, &position, disk->nvme_nsid) < 0)
            return -22;
    } else {
        if (storage_path_append_text(out, capacity, &position, "/dev/sd") < 0 ||
            storage_append_sd_suffix(out, capacity, &position,
                                     disk->name_index) < 0)
            return -22;
    }
    if (partition_index >= 0) {
        if (disk->transport == STORAGE_TRANSPORT_NVME &&
            storage_path_append_text(out, capacity, &position, "p") < 0)
            return -22;
        if (storage_path_append_u32(out, capacity, &position,
                                    (uint32_t)partition_index + 1u) < 0)
            return -22;
    }
    return 0;
}

/** @brief Resolve a standard device basename to its current admitted identity.
 * @param name Borrowed NUL-terminated basename without /dev prefix.
 * @param disk_id Required stable inventory/optical ID output, valid on success.
 * @param partition_index Required GPT index/whole-device selector output.
 * @return Zero or negative input/metadata allocation/registry errno. Execution
 * ownership; refresh may grow core metadata outside registry locks, never does
 * native I/O or leaves a provider pointer/reference in the returned identity.
 */
int storage_parse_block_name(const char *name, uint32_t *disk_id,
                             int32_t *partition_index)
{
    char candidate[RELIEFOS_FS_PATH_LEN];
    if (!name || !disk_id || !partition_index) return -22;
    int refreshed=storage_refresh_disks();
    for (uint32_t i = 0; i < g_install_disk_count; ++i) {
        for (int32_t part = -1; part < (int32_t)RELIEFOS_DISK_MAX_PARTITIONS; ++part) {
            if (storage_disk_device_name(i, part, candidate, sizeof(candidate)) < 0) continue;
            if (!storage_text_eq(name, candidate + 5)) continue;
            *disk_id = i;
            *partition_index = part;
            return 0;
        }
    }
    uint64_t cursor=0;uint32_t optical;int rc;
    while((rc=storage_optical_next(&cursor,&optical))>0) {
        if(!storage_disk_device_name(optical,-1,candidate,sizeof(candidate)) && storage_text_eq(name,candidate+5)) {
            *disk_id=optical;*partition_index=-1;return 0;
        }
    }
    if(rc<0)return rc;
    return refreshed ? refreshed : -22;
}

/**
 * @brief Discover the reserved ESP instance using the runtime root's transport.
 * @return Zero with a ready native FAT instance or negative discovery/mount errno.
 * Boot-only, before concurrent I/O; restores active-volume selection. Copies
 * geometry, never journal ownership. The initial VFS graph attaches /boot later.
 */
static int storage_mount_runtime_boot(void)
{
    struct storage_volume *root = &g_volumes[STORAGE_VOLUME_ROOT];
    struct storage_volume *boot = &g_volumes[STORAGE_VOLUME_BOOT];
    struct storage_volume *old = g_active_volume;
    int ret;
    if (!root->ready || root->transport == 0 || root->esp_sector_count == 0) {
        return -2;
    }
    storage_destroy_volume(boot);
    storage_memzero(boot, sizeof(*boot));
    ret=storage_volume_from_install_disk(boot,&g_install_disks[root->source_disk_id]);
    if(ret)return ret;
    boot->esp_start_lba=root->esp_start_lba;boot->esp_sector_count=root->esp_sector_count;
    boot->esp_device=root->esp_device;
    __builtin_memcpy(boot->esp_unique_guid,root->esp_unique_guid,sizeof(boot->esp_unique_guid));
    /* Copy transport geometry, never ownership of the root journal. */
    storage_memzero(&boot->ext4, sizeof(boot->ext4));
    boot->read_only_reason = STORAGE_EXT4_READ_ONLY_NONE;
    boot->volume_id = STORAGE_VOLUME_BOOT;
    boot->ready = false;
    boot->filesystem = STORAGE_FILESYSTEM_NONE;
    boot->data_partition_mount = 0;
    g_active_volume = boot;
    ret = storage_mount_filesystem(boot,"vfat");
    if (ret == 0) {
        boot->ready = true;
    } else {
        storage_destroy_volume(boot);
        storage_memzero(boot, sizeof(*boot));
    }
    g_active_volume = old;
    storage_cache_invalidate();
    return ret;
}

/** @brief Probe an ext-family root through registered feature-checked code.
 * @param root Exclusive unpublished native context with ext partition geometry.
 * @return Zero with an instance-owned provider reference or negative errno.
 * BSP boot execution only; provider admission precedes parsing/I/O and cleanup
 * restores failure ownership without a global pathname or callback lock.
 */
static int storage_mount_ext_family(struct storage_volume *root)
{ return storage_mount_filesystem(root,"ext4"); }

/**
 * @brief Probe one borrowed discovered disk as the reserved runtime-root instance.
 * @param disk Required persistent transport record; remains owned by discovery.
 * @return Zero for a ready native root, or negative partition/filesystem errno.
 * BSP boot-only, before concurrent I/O. Restores active selection on failure;
 * success retains native filesystem state, never a namespace pathname.
 */
static int storage_try_mount_root_disk(struct install_disk_state *disk)
{
    struct storage_volume *root = &g_volumes[STORAGE_VOLUME_ROOT];
    struct storage_volume *old = g_active_volume;
    int ret;
    if (!disk || !disk->present) {
        return -22;
    }
    storage_destroy_volume(root);
    storage_memzero(root, sizeof(*root));
    root->volume_id = STORAGE_VOLUME_ROOT;
    ret=storage_volume_from_install_disk(root,disk);
    if(ret)return ret;
    g_active_volume = root;
    ret = gpt_find_esp();
    if (ret == 0) {
        if (root->exfat_start_lba) {
            /* A named exFAT root is authoritative. Never silently mount the
             * ESP as / when its metadata is corrupt: that hides the real
             * failure and starts an incomplete system. */
            console_info("[reliefnt] attempting exFAT mount lba=%llu sectors=%u\n",
                           (unsigned long long)root->exfat_start_lba,
                           (unsigned int)root->exfat_sector_count);
            ret = storage_mount_filesystem(root,"exfat");
            console_log(ret < 0 ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
                        "[reliefnt] exfat_mount returned %d\n", ret);
            if (ret < 0 && root->ext_start_lba) {
                ret = storage_mount_ext_family(root);
            }
        } else if (root->ext_start_lba) {
            ret = storage_mount_ext_family(root);
        } else {
            /* A GPT disk with an ESP but no recognized root must fail
             * explicitly. Mounting the ESP as / leaves a seemingly bootable
             * system with no /system tree and hides the real storage error. */
            console_warn("[reliefnt] GPT root filesystem not identified; refusing ESP fallback\n");
            ret = -2;
        }
    }
    if (ret == 0) {
        root->ready = true;
        if (storage_mount_runtime_boot() < 0) {
            console_warn("[reliefnt] storage boot ESP mount unavailable\n");
        }
        disk->boot_root = 1;
        console_info("[reliefnt] storage ready transport=%s pci=%u:%u.%u unit=%u root=%s\n",
                       storage_transport_name(disk->transport),
                       disk->bus, disk->slot, disk->function,
                       disk->transport == STORAGE_TRANSPORT_NVME ? disk->nvme_nsid : disk->port,
                       root->filesystem == STORAGE_FILESYSTEM_EXFAT ? "exfat" :
                       (root->filesystem == STORAGE_FILESYSTEM_EXT4 ? "ext4" :
                        (root->filesystem == STORAGE_FILESYSTEM_EXT2 ? "ext2" : "fat32")));
    } else {
        /* Failed root mount: release the ext4 state and its caches before
         * the volume slot is reused; g_active_volume is restored below. */
        storage_destroy_volume(root);
        storage_memzero(root, sizeof(*root));
    }
    g_active_volume = old;
    storage_cache_invalidate();
    return ret;
}

/** @brief Retire unpublished boot volumes and reset storage execution state.
 * @return None. BSP exclusive setup before namespace/inode/task users. Runs
 * each native destructor before releasing code/device ownership and refreshes
 * scalar inventory without selecting a root, probing hardware or resetting
 * controllers. No graph/module callback lock spans cleanup or allocation.
 */
void storage_boot_reset(void)
{
    storage_boot_drop_instances();
    storage_memzero(g_volumes,sizeof(g_volumes));
    storage_disk_block_cache_reset();storage_refresh_disks();
    g_devfs_enabled=1;g_installer_root_active=0;
    storage_io_async_context=false;storage_io_write_started=false;
    g_active_volume=&g_volumes[0];storage_cache_invalidate();
}

/** @brief Discover a direct-boot root and optical mounts from registered devices.
 * @return None. BSP exclusive setup before namespace/inode users. Resets only
 * unpublished storage instances, never controllers. Every successful parser
 * retains independent code/device ownership, no registry lock spans disk I/O.
 */
void storage_init(void)
{
    storage_boot_reset();
    bool explicit_root=storage_boot_root_requested(),root_ready=false;
    if(explicit_root) {
        int selected=storage_boot_root_select();
        if(selected>=0) {
            storage_boot_root_filter_enable(true);
            root_ready=storage_try_mount_root_disk(&g_install_disks[selected])==0;
            storage_boot_root_filter_enable(false);
        }
        if(!root_ready)console_error("[reliefnt] explicit root selection failed status=%d; refusing automatic fallback\n",selected<0 ? selected : -2);
    } else for(uint32_t i=0;i<g_install_disk_count && !root_ready;++i)
        if(g_install_disks[i].present)root_ready=storage_try_mount_root_disk(&g_install_disks[i])==0;
    uint64_t cursor=0;struct block_device_ref ref={0};uint32_t slot=STORAGE_VOLUME_DYNAMIC_FIRST;
    while(slot<STORAGE_MAX_VOLUMES && !block_device_next(&cursor,&ref)) {
        struct block_device_info info;
        if(!block_device_info(&ref,&info) && (info.flags&BLOCK_DEVICE_OPTICAL)) {
            struct storage_volume *volume=&g_volumes[slot];
            struct install_disk_state disk={.present=true,.device=info.device};
            volume->volume_id=slot;
            volume->source_disk_id=STORAGE_BLOCK_OPTICAL_BIT|(info.device&255u)|((info.device>>12)&0xfffff00u);
            volume->source_partition_index=UINT32_MAX;
            int rc=storage_volume_from_install_disk(volume,&disk);
            if(!rc)rc=storage_mount_filesystem(volume,"iso9660");
            if(!rc){volume->ready=true;++slot;}
            else {storage_destroy_volume(volume);*volume=(struct storage_volume){0};}
        }
        block_device_put(&ref);
    }
    g_active_volume=&g_volumes[0];
    if(!root_ready)console_error("[reliefnt] no registered GPT root filesystem found\n");
}

/**
 * @brief Look up a named Multiboot module and return its byte range.
 * @param boot Parsed boot information; may be NULL.
 * @param name Exact module name to find.
 * @param out_start Output start address when the module is present.
 * @param out_length Output length in bytes when the module is present.
 * @return True when a module with that name and a non-empty range was found.
 */
static bool boot_find_module_range(const struct boot_info *boot, const char *name,
                                   uint64_t *out_start, uint64_t *out_length)
{
    if (!boot) {
        return false;
    }
    for (uint32_t i = 0;
         i < boot->module_count && i < sizeof(boot->modules) / sizeof(boot->modules[0]);
         ++i) {
        const struct boot_module *module = &boot->modules[i];
        if (storage_text_eq(module->name, name) && module->end > module->start) {
            *out_start = module->start;
            *out_length = module->end - module->start;
            return true;
        }
    }
    return false;
}

/**
 * @brief Mount the root filesystem for this boot.
 * @param boot Parsed Multiboot modules and kernel command line.
 * @param ramdisk_root True for an installer or live session, whose root is the
 *        `reliefos-installer-root` module rather than a mounted partition;
 *        the legacy `leonos-installer-root` tag remains accepted.
 *
 * The physical disks are always probed first: an installer needs the disk and
 * partition inventory even though its own root comes from RAM. When the RAM
 * image cannot be mounted the volume table is cleared, so no stale disk root
 * is presented to userland; the caller then retries through
 * storage_init_installer_root(), which reports the reason.
 */
void storage_mount_boot_root(const struct boot_info *boot, bool ramdisk_root)
{
    uint64_t start = 0;
    uint64_t length = 0;
    storage_boot_root_configure(!ramdisk_root && boot ? boot->cmdline : NULL);
    storage_init();
    if (!ramdisk_root) {
        const char *root_fs = g_volumes[STORAGE_VOLUME_ROOT].filesystem == STORAGE_FILESYSTEM_EXT4
                                  ? "ext4"
                                  : storage_root_filesystem_name();
        console_info("[reliefnt] storage boot root from probed disks root=%s fs=%s\n",
                       root_fs, root_fs);
        return;
    }
    if (!boot_find_module_range(boot, "reliefos-installer-root", &start, &length) &&
        !boot_find_module_range(boot, "leonos-installer-root", &start, &length)) {
        /* storage_init() probes a physical root before the installer module is
         * selected.  Do not leave that disk mounted as / when the RAM root is
         * absent: an installer would then operate on its own boot media and
         * may present a partially valid system instead of failing closed. */
        storage_destroy_volume(&g_volumes[STORAGE_VOLUME_ROOT]);
        storage_memzero(&g_volumes[STORAGE_VOLUME_ROOT],
                        sizeof(g_volumes[STORAGE_VOLUME_ROOT]));
        storage_destroy_volume(&g_volumes[STORAGE_VOLUME_BOOT]);
        storage_memzero(&g_volumes[STORAGE_VOLUME_BOOT],
                        sizeof(g_volumes[STORAGE_VOLUME_BOOT]));
        g_active_volume = &g_volumes[STORAGE_VOLUME_ROOT];
        g_installer_root_active = 0;
        storage_cache_invalidate();
        console_warn("[reliefnt] storage boot root has no installer module; physical root cleared\n");
        return;
    }
    if (storage_mount_ramdisk_root(start, length) < 0) {
        storage_boot_drop_instances();
        storage_memzero(g_volumes, sizeof(g_volumes));
        g_active_volume = &g_volumes[0];
        console_warn("[reliefnt] storage installer ramdisk root not mounted bytes=%llu\n",
                       (unsigned long long)length);
        return;
    }
    /* Installer mode owns / and later reuses the boot slot for the selected
     * target ESP at /target/boot. */
    storage_destroy_volume(&g_volumes[STORAGE_VOLUME_BOOT]);
    storage_memzero(&g_volumes[STORAGE_VOLUME_BOOT], sizeof(g_volumes[STORAGE_VOLUME_BOOT]));
    g_active_volume = &g_volumes[STORAGE_VOLUME_ROOT];
    console_info("[reliefnt] storage boot root from installer ramdisk root=/\n");
}

/** @brief Mount the reserved installer image through the shared block registry.
 * @param image_phys Reserved physical source, never a CPU or DMA pointer.
 * @param len Sector-aligned reserved source length, retained through shutdown.
 * @return Zero or source/geometry/allocation/provider errno. BSP exclusive setup
 * before namespace users; a failed selection clears the unpublished root.
 * Registration borrows the existing direct map without copying the image. All
 * filesystem I/O then uses the same owned block path as a physical disk; writes
 * change only session memory. No graph/registry lock spans native parsing.
 */
int storage_mount_ramdisk_root(uint64_t image_phys, uint64_t len)
{
    struct storage_volume *root=&g_volumes[STORAGE_VOLUME_ROOT];
    storage_destroy_volume(root);*root=(struct storage_volume){0};
    g_active_volume=root;g_installer_root_active=0;storage_cache_invalidate();
    uint64_t device;int rc=block_boot_image_bind(image_phys,len,&device);if(rc)return rc;
    rc=storage_refresh_disks();if(rc)return rc;
    uint32_t disk=0;
    while(disk<g_install_disk_count && g_install_disks[disk].device!=device)++disk;
    if(disk==g_install_disk_count)return -19;
    const char *filesystem="vfat";
    if(len>=4096) {
        uint8_t sector[512];struct block_device_ref ref={0};
        rc=block_device_get(device,&ref);
        if(!rc)rc=block_device_read(&ref,2,1,sector);
        block_device_put(&ref);if(rc)return rc;
        if(sector[56]==0x53 && sector[57]==0xef)filesystem="ext4";
    }
    rc=storage_mount_whole_filesystem(root,disk,filesystem);
    if(rc){*root=(struct storage_volume){0};return rc;}
    for(uint32_t i=0;i<g_install_disk_count;++i)g_install_disks[i].boot_root=0;
    g_installer_root_active=1;
    console_info("[reliefnt] installer RAM block root ready device=ram0 bytes=%llu fs=%s\n",
                 (unsigned long long)len,storage_root_filesystem_name());
    return 0;
}

/**
 * @brief Mount the named installer-root module from Multiboot physical memory.
 * @param boot Parsed Multiboot module list; may be NULL, in which case no
 *             module is mounted and the root remains unavailable.
 * @return Nothing. Missing or invalid module data is logged and leaves the
 *         root volume cleared instead of retaining a physical-disk root.
 * @par Context
 * Boot setup before tasks can issue storage operations. The module's
 *          physical range remains reserved and is mapped by the mount routine.
 */
void storage_init_installer_root(const struct boot_info *boot)
{
    int mount_ret = -2;
    bool found = false;
    storage_boot_drop_instances();
    storage_memzero(g_volumes, sizeof(g_volumes));
    storage_disk_block_cache_reset();
    g_active_volume = &g_volumes[0];
    g_installer_root_active = 0;
    storage_cache_invalidate();
    if (boot) {
        for (uint32_t i = 0; i < boot->module_count; ++i) {
            const struct boot_module *mod = &boot->modules[i];
            if (storage_text_eq(mod->name, "reliefos-installer-root") ||
                storage_text_eq(mod->name, "leonos-installer-root")) {
                found = true;
                if (mod->end > mod->start) {
                    mount_ret = storage_mount_ramdisk_root(mod->start,
                                                          mod->end - mod->start);
                    if (mount_ret == 0) {
                        return;
                    }
                } else {
                    mount_ret = -22;
                }
                console_error("[reliefnt] installer root ramdisk module mount failed ret=%d bytes=%llu\n",
                               mount_ret,
                               (unsigned long long)(mod->end > mod->start
                                   ? mod->end - mod->start : 0));
                return;
            }
        }
    }
    if (!found) {
        console_warn("[reliefnt] installer root ramdisk module not found; ensure GRUB had enough RAM to load /install/root.fat\n");
    }
}

#include "internal.h"


const struct storage_dev_entry storage_dev_entries[] = {
    {"null",      STORAGE_DEV_KIND_NULL,     RELIEFOS_FS_TYPE_DEVICE, 0},
    {"zero",      STORAGE_DEV_KIND_ZERO,     RELIEFOS_FS_TYPE_DEVICE, 0},
    {"full",      STORAGE_DEV_KIND_FULL,     RELIEFOS_FS_TYPE_DEVICE, 0},
    {"random",    STORAGE_DEV_KIND_RANDOM,   RELIEFOS_FS_TYPE_DEVICE, 0},
    {"urandom",   STORAGE_DEV_KIND_URANDOM,  RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty",       STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty0",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty1",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty2",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty3",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty4",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty5",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"tty6",      STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"console",   STORAGE_DEV_KIND_CONSOLE,  RELIEFOS_FS_TYPE_DEVICE, 0},
    {"ptmx",      STORAGE_DEV_KIND_PTMX,     RELIEFOS_FS_TYPE_DEVICE, 0},
    {"fb0",       STORAGE_DEV_KIND_FB0,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"keyboard",  STORAGE_DEV_KIND_KEYBOARD, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mouse",     STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"dsp",       STORAGE_DEV_KIND_AUDIO,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mixer",     STORAGE_DEV_KIND_AUDIO_MIXER, RELIEFOS_FS_TYPE_DEVICE, 0},
    /* Compatibility aliases for the retired private audio interface. */
    {"audio",     STORAGE_DEV_KIND_AUDIO,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"ttyS0",     STORAGE_DEV_KIND_SERIAL,   RELIEFOS_FS_TYPE_DEVICE, 0},
    {"serial0",   STORAGE_DEV_KIND_SERIAL,   RELIEFOS_FS_TYPE_DEVICE, 0},
    {"ethernet0", STORAGE_DEV_KIND_NET,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"rtc",       STORAGE_DEV_KIND_RTC,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"driverctl", STORAGE_DEV_KIND_DRIVERCTL, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"kmsg",      STORAGE_DEV_KIND_KMSG,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"gpu",         STORAGE_DEV_KIND_GPU,          RELIEFOS_FS_TYPE_DEVICE, 0},
    {"shm0",       STORAGE_DEV_KIND_SHM,          RELIEFOS_FS_TYPE_DEVICE, 0},
    {"stdin",     STORAGE_DEV_KIND_TTY,      RELIEFOS_FS_TYPE_DEVICE, 0},
    {"stdout",    STORAGE_DEV_KIND_CONSOLE,  RELIEFOS_FS_TYPE_DEVICE, 0},
    {"stderr",    STORAGE_DEV_KIND_CONSOLE,  RELIEFOS_FS_TYPE_DEVICE, 0},
    {"input",     STORAGE_DEV_KIND_INPUT_DIR, RELIEFOS_FS_TYPE_DIR, 1},
    {"snd",       STORAGE_DEV_KIND_SND_DIR,   RELIEFOS_FS_TYPE_DIR, 1},
    {"disk",      STORAGE_DEV_KIND_DISK_DIR, RELIEFOS_FS_TYPE_DIR, 1},
    {"pts",       STORAGE_DEV_KIND_PTS_DIR,   RELIEFOS_FS_TYPE_DIR, 1},
    {"shm",       0,                          RELIEFOS_FS_TYPE_DIR, 1},
};

const struct storage_dev_entry storage_dev_input_entries[] = {
    {"event0",    STORAGE_DEV_KIND_KEYBOARD, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"event1",    STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"keyboard",  STORAGE_DEV_KIND_KEYBOARD, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mouse0",    STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mouse",     STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
};

bool storage_ready(void)
{
    return g_volumes[0].ready;
}

void storage_format_u32(char *out, uint32_t cap, const char *prefix,
                               uint32_t value, int partition)
{
    char digits[11];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!out || cap == 0) return;
    if (prefix) {
        while (prefix[pos] && pos + 1u < cap) {
            out[pos] = prefix[pos];
            ++pos;
        }
    }
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(digits));
    while (n && pos + 1u < cap) out[pos++] = digits[--n];
    if (partition >= 0 && pos + 2u < cap) {
        out[pos++] = 'p';
        value = (uint32_t)partition + 1u;
        n = 0;
        do {
            digits[n++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value && n < sizeof(digits));
        while (n && pos + 1u < cap) out[pos++] = digits[--n];
    }
    out[pos] = 0;
}

/**
 * @brief Reports the filesystem implementation backing the runtime root at runtime.
 * @return A static lowercase name suitable for boot diagnostics.
 */
const char *storage_root_filesystem_name(void)
{
    switch (g_volumes[0].filesystem) {
    case STORAGE_FILESYSTEM_EXFAT:
        return "exfat";
    case STORAGE_FILESYSTEM_EXT2:
        return "ext2";
    case STORAGE_FILESYSTEM_EXT4:
        return "ext4";
    case STORAGE_FILESYSTEM_FAT32:
        return "fat32";
    case STORAGE_FILESYSTEM_ISO9660:
        return "iso9660";
    default:
        return "none";
    }
}

bool storage_installer_root_active(void)
{
    return g_installer_root_active != 0;
}

int storage_resolve_path(const char *cwd, const char *input, char *out, uint32_t cap)
{
    char parts[16][RELIEFOS_FS_NAME_LEN];
    uint32_t part_count = 0;
    const char *sources[2];
    uint32_t source_count;
    if (!input || !out || cap < 2) {
        return -22;
    }
    /* POSIX permits ':' in a pathname component (xdm uses A:0-XXXXXX for
     * authority files). Backslash remains rejected as a non-POSIX separator
     * and to avoid accepting DOS drive/escape syntax accidentally. */
    for (uint32_t i = 0; input[i]; ++i) {
        if (input[i] == '\\') {
            return -22;
        }
    }
    if (input[0] == '/') {
        sources[0] = input + 1;
        source_count = 1;
    } else {
        if (!cwd || cwd[0] != '/') {
            cwd = "/";
        }
        for (uint32_t i = 0; cwd[i]; ++i) {
            if (cwd[i] == '\\') {
                return -22;
            }
        }
        sources[0] = cwd + 1;
        sources[1] = input;
        source_count = 2;
    }
    for (uint32_t src_i = 0; src_i < source_count; ++src_i) {
        const char *p = sources[src_i];
        char token[RELIEFOS_FS_NAME_LEN];
        uint32_t pos = 0;
        while (1) {
            char ch = *p;
            if (ch == '/' || ch == 0) {
                token[pos] = 0;
                if (pos != 0) {
                    if (storage_text_eq(token, ".")) {
                    } else if (storage_text_eq(token, "..")) {
                        if (part_count) {
                            --part_count;
                        }
                    } else if (part_count < 16) {
                        storage_copy_text(parts[part_count++], sizeof(parts[0]), token);
                    } else {
                        return -22;
                    }
                }
                pos = 0;
                if (ch == 0) {
                    break;
                }
                ++p;
                continue;
            }
            if (pos + 1 >= sizeof(token)) {
                return -22;
            }
            token[pos++] = ch;
            ++p;
        }
    }

    uint32_t out_pos = 0;
    out[out_pos++] = '/';
    out[out_pos] = 0;
    for (uint32_t i = 0; i < part_count; ++i) {
        if (out_pos + storage_strlen(parts[i]) + 1 >= cap) {
            return -22;
        }
        if (out_pos > 1) {
            out[out_pos++] = '/';
        }
        for (uint32_t j = 0; parts[i][j]; ++j) {
            out[out_pos++] = parts[i][j];
        }
        out[out_pos] = 0;
    }
    return 0;
}

int storage_lookup_path_unlocked(const char *path, struct storage_node *out)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    struct storage_volume *volume;
    int ret;
    if (!storage_ready()) {
        return -2;
    }
    if (storage_resolve_path("/", path, resolved, sizeof(resolved)) < 0) {
        return -22;
    }
    if (g_devfs_enabled && !__builtin_strncmp(resolved, "/dev/pts/", 9))
        return pty_lookup_path(resolved, out);
    if (g_devfs_enabled && !__builtin_strncmp(resolved, "/dev/tty", 8) &&
        resolved[8] >= '0' && resolved[8] <= '6' && !resolved[9])
        return pty_lookup_vt_path(resolved, out);
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev")) {
        if (out) {
            *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_DEV_DIR,
                .first_cluster = STORAGE_DEV_KIND_DIR,
                .volume_id = STORAGE_VOLUME_ROOT,
                .size = 0,
            };
        }
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev/input")) {
        if (out) {
            *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_DEV_DIR,
                .first_cluster = STORAGE_DEV_KIND_INPUT_DIR,
                .volume_id = STORAGE_VOLUME_ROOT,
                .size = 0,
            };
        }
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev/pts")) {
        if (out) {
            *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_DEV_DIR,
                .first_cluster = STORAGE_DEV_KIND_PTS_DIR,
                .volume_id = STORAGE_VOLUME_ROOT,
                .size = 0,
            };
        }
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev/snd")) {
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR,
            .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = STORAGE_DEV_KIND_SND_DIR,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev/dri")) {
        if (!drm_device_is_registered()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR,
            .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = STORAGE_DEV_KIND_DRI_DIR,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev/dri/card0")) {
        if (!drm_device_is_registered()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DEVICE,
            .flags = STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster = STORAGE_DEV_KIND_DRM_CARD0,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (g_devfs_enabled && storage_text_eq_ci(resolved,
                                               "/dev/dri/renderD128")) {
        if (!drm_device_render_node_ready()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DEVICE,
            .flags = STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster = STORAGE_DEV_KIND_DRM_RENDERD128,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (g_devfs_enabled && !__builtin_strncmp(resolved, "/dev/snd/", 9) &&
        resolved[9]) {
        ret = storage_audio_timer_node(resolved + 9, out);
        if (ret == 0) return 0;
        ret = storage_audio_control_node(resolved + 9, out);
        if (ret == 0) return 0;
        ret = storage_audio_pcm_node(resolved + 9, out);
        if (ret == 0) return 0;
        return -2;
    }
    if (g_devfs_enabled && (storage_text_eq(resolved, "/dev/disk") ||
                            storage_text_eq(resolved, "/dev/disk/by-partuuid"))) {
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR, .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = storage_text_eq(resolved, "/dev/disk")
                ? STORAGE_DEV_KIND_DISK_DIR : STORAGE_DEV_KIND_PARTUUID_DIR,
        };
        return 0;
    }
    if (g_devfs_enabled && !__builtin_strncmp(resolved, "/dev/disk/by-partuuid/", 22)) {
        char target[48];
        ret = storage_devlink_target(resolved, target);
        if (!ret && out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_SYMLINK, .flags = STORAGE_NODE_FLAG_DEV_LINK,
            .size = storage_strlen(target),
        };
        return ret;
    }
    /* /dev/shm is a real writable directory in the root backend. Boot resets
     * its contents before userspace; do not synthesize a read-only dev node. */
    if (g_devfs_enabled && !storage_text_eq(resolved, "/dev/shm") &&
        __builtin_strncmp(resolved, "/dev/shm/", 9) != 0 && storage_strlen(resolved) > 5u &&
        (resolved[0] == '/' && (resolved[1] == 'd' || resolved[1] == 'D') &&
         (resolved[2] == 'e' || resolved[2] == 'E') &&
         (resolved[3] == 'v' || resolved[3] == 'V') && resolved[4] == '/')) {
        const char *name = resolved + 5;
        const struct storage_dev_entry *entry = NULL;
        if (storage_strlen(name) > 6u &&
            (name[0] == 'i' || name[0] == 'I') &&
            (name[1] == 'n' || name[1] == 'N') &&
            (name[2] == 'p' || name[2] == 'P') &&
            (name[3] == 'u' || name[3] == 'U') &&
            (name[4] == 't' || name[4] == 'T') && name[5] == '/') {
            entry = storage_dev_find(name + 6, storage_dev_input_entries,
                                     (uint32_t)(sizeof(storage_dev_input_entries) /
                                                sizeof(storage_dev_input_entries[0])));
        } else if (!storage_text_eq_ci(name, "input") &&
                   !storage_text_eq_ci(name, "pts") &&
                   !storage_strlen(name)) {
            entry = NULL;
        } else if (!storage_text_eq_ci(name, "input") &&
                   !storage_text_eq_ci(name, "pts")) {
            entry = storage_dev_find(name, storage_dev_entries,
                                     (uint32_t)(sizeof(storage_dev_entries) /
                                                sizeof(storage_dev_entries[0])));
        }
        if (entry && !entry->directory && out) {
            storage_dev_node(entry, out);
            return 0;
        }
        if (entry && entry->directory) {
            return -20;
        }
        /* GPT disks and partitions are generated by the block layer rather
         * than stored in the fixed devfs alias table. */
        {
            uint32_t disk_id;
            int32_t partition_index;
            uint64_t first_lba;
            uint64_t sector_count;
            int parsed = storage_parse_block_name(name, &disk_id, &partition_index);
            int block_ret = parsed;
            if (parsed == 0)
                block_ret = storage_disk_block_info(disk_id, partition_index,
                                                    &first_lba, &sector_count);
            if (block_ret == 0) {
                if (out) {
                    *out = (struct storage_node){
                        .type = RELIEFOS_FS_TYPE_DEVICE,
                        .flags = STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_BLOCK,
                        .first_cluster = STORAGE_DEV_KIND_DISK,
                        .volume_id = STORAGE_BLOCK_VOLUME_ID(disk_id, partition_index),
                        .size = sector_count * 512u,
                    };
                }
                return 0;
            }
            if (parsed == 0) return block_ret;
        }
        return -2;
    }

    ret = storage_route_path(resolved, &volume, backend_path, sizeof(backend_path));
    if (ret < 0) {
        return ret;
    }
    ret = storage_select_volume(volume->volume_id);
    if (ret < 0) {
        return ret;
    }

    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS)
        return tmpfs_lookup(g_storage.tmpfs, backend_path, out);
    if (storage_path_cache_lookup(resolved, out)) {
        return 0;
    }

    if (storage_ext4_is_ext_family(&g_storage)) {
        ret = storage_ext4_ops.lookup(&g_storage, backend_path, out);
        if (ret == 0 && out) storage_path_cache_store(resolved, out);
        return ret;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        ret = exfat_lookup_path(backend_path, out);
        if (ret == 0 && out) storage_path_cache_store(resolved, out);
        return ret;
    }

    struct storage_node node = {
        .type = RELIEFOS_FS_TYPE_DIR,
        .flags = STORAGE_NODE_FLAG_ROOT,
        .first_cluster = g_storage.filesystem == STORAGE_FILESYSTEM_ISO9660
                             ? g_storage.iso_root_extent : g_storage.root_cluster,
        .volume_id = g_storage.volume_id,
        .size = g_storage.filesystem == STORAGE_FILESYSTEM_ISO9660
                    ? g_storage.iso_root_size : 0,
    };
    const char *p = backend_path + 1;
    char name[RELIEFOS_FS_NAME_LEN];
    uint32_t pos = 0;
    while (1) {
        char ch = *p;
        if (ch == '/' || ch == 0) {
            name[pos] = 0;
            if (pos) {
                if (node.type != RELIEFOS_FS_TYPE_DIR || node.flags == STORAGE_NODE_FLAG_DEV_DIR) {
                    return -20;
                }
                int ret = g_storage.filesystem == STORAGE_FILESYSTEM_ISO9660
                              ? iso9660_find_in_dir(node.first_cluster, (uint32_t)node.size,
                                                    name, &node)
                              : fat32_find_in_dir(node.first_cluster, name, &node);
                if (ret < 0) {
                    return ret;
                }
            }
            pos = 0;
            if (ch == 0) {
                break;
            }
            ++p;
            continue;
        }
        if (pos + 1 >= sizeof(name)) {
            return -22;
        }
        char out_ch = ch;
        if (out_ch >= 'A' && out_ch <= 'Z') {
            out_ch = (char)(out_ch - 'A' + 'a');
        }
        name[pos++] = out_ch;
        ++p;
    }
    if (out) {
        *out = node;
    }
    storage_path_cache_store(resolved, &node);
    return 0;
}

int storage_lookup_path(const char *path, struct storage_node *out)
{
    int ret;
    uint64_t flags;
    /* Filesystem metadata, caches, the active-volume selector and the
     * polled AHCI command path are shared by all CPUs.  Use the reentrant
     * execution transaction shared with syscalls and page faults, so lazy
     * ELF loading cannot observe a write transaction halfway through. */
    kernel_execution_lock_irqsave(&flags);
    ret = storage_lookup_path_unlocked(path, out);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

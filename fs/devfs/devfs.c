#include <reliefnt/sound_service.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/graphics_service.h>

struct storage_dev_entry {
    const char *name;
    uint32_t kind,type;
    uint8_t directory;
};

static const struct storage_dev_entry storage_dev_entries[] = {
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

static const struct storage_dev_entry storage_dev_input_entries[] = {
    {"event0",    STORAGE_DEV_KIND_KEYBOARD, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"event1",    STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"keyboard",  STORAGE_DEV_KIND_KEYBOARD, RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mouse0",    STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
    {"mouse",     STORAGE_DEV_KIND_MOUSE,    RELIEFOS_FS_TYPE_DEVICE, 0},
};

static const struct storage_dev_entry *storage_dev_find(const char *name,
                                                         const struct storage_dev_entry *entries,
                                                         uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (storage_text_eq_ci(name, entries[i].name)) {
            return &entries[i];
        }
    }
    return NULL;
}

static void storage_dev_node(const struct storage_dev_entry *entry,
                             struct storage_node *out)
{
    if (!entry || !out) {
        return;
    }
    *out = (struct storage_node){
        .type = entry->type,
        .flags = entry->directory ? STORAGE_NODE_FLAG_DEV_DIR
                                  : (STORAGE_NODE_FLAG_DEV_NODE |
                                     (entry->kind == STORAGE_DEV_KIND_FB0
                                          ? STORAGE_NODE_FLAG_DEV_FB0 : 0u) |
                                     (entry->kind == STORAGE_DEV_KIND_AUDIO_MIXER
                                          ? STORAGE_NODE_FLAG_AUDIO_MIXER : 0u)),
        .first_cluster = entry->kind,
        .volume_id = STORAGE_VOLUME_ROOT,
        .size = 0,
    };
}

int storage_devfs_lookup(const char *resolved,struct storage_node *out)
{
    int ret;
    if (!__builtin_strncmp(resolved, "/dev/pts/", 9))
        return pty_lookup_path(resolved, out);
    if (!__builtin_strncmp(resolved, "/dev/tty", 8) &&
        resolved[8] >= '0' && resolved[8] <= '6' && !resolved[9])
        return pty_lookup_vt_path(resolved, out);
    if (storage_text_eq_ci(resolved, "/dev")) {
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
    if (storage_text_eq_ci(resolved, "/dev/input")) {
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
    if (storage_text_eq_ci(resolved, "/dev/pts")) {
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
    if (storage_text_eq_ci(resolved, "/dev/snd")) {
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR,
            .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = STORAGE_DEV_KIND_SND_DIR,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (storage_text_eq_ci(resolved, "/dev/dri")) {
        if (!graphics_service_is_registered()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR,
            .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = STORAGE_DEV_KIND_DRI_DIR,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (storage_text_eq_ci(resolved, "/dev/dri/card0")) {
        if (!graphics_service_is_registered()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DEVICE,
            .flags = STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster = STORAGE_DEV_KIND_DRM_CARD0,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (storage_text_eq_ci(resolved,
                                               "/dev/dri/renderD128")) {
        if (!graphics_service_render_node_ready()) return -2;
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DEVICE,
            .flags = STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster = STORAGE_DEV_KIND_DRM_RENDERD128,
            .volume_id = STORAGE_VOLUME_ROOT,
        };
        return 0;
    }
    if (!__builtin_strncmp(resolved, "/dev/snd/", 9) &&
        resolved[9]) {
        ret = sound_service_timer_node(resolved + 9, out);
        if (ret == 0) return 0;
        ret = sound_service_control_node(resolved + 9, out);
        if (ret == 0) return 0;
        ret = sound_service_pcm_node(resolved + 9, out);
        if (ret == 0) return 0;
        return -2;
    }
    if ((storage_text_eq(resolved, "/dev/disk") ||
                            storage_text_eq(resolved, "/dev/disk/by-partuuid"))) {
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_DIR, .flags = STORAGE_NODE_FLAG_DEV_DIR,
            .first_cluster = storage_text_eq(resolved, "/dev/disk")
                ? STORAGE_DEV_KIND_DISK_DIR : STORAGE_DEV_KIND_PARTUUID_DIR,
        };
        return 0;
    }
    if (!__builtin_strncmp(resolved, "/dev/disk/by-partuuid/", 22)) {
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
    if (!storage_text_eq(resolved, "/dev/shm") &&
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

    return -2;
}

int storage_devfs_readdir_node(const struct storage_node *node,uint64_t *cursor,
                                 struct reliefos_dir_entry *entry)
{
    if(!node || !cursor || !entry)return -22;
    if(node->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    if(node->first_cluster==DEVFS_SHM_DIRECTORY)return 0;
    if (node->flags & STORAGE_NODE_FLAG_DEV_DIR) {
        const struct storage_dev_entry *entries = storage_dev_entries;
        uint32_t count = (uint32_t)(sizeof(storage_dev_entries) /
                                     sizeof(storage_dev_entries[0]));
        if (node->first_cluster == STORAGE_DEV_KIND_INPUT_DIR) {
            entries = storage_dev_input_entries;
            count = (uint32_t)(sizeof(storage_dev_input_entries) /
                               sizeof(storage_dev_input_entries[0]));
        } else if (node->first_cluster == STORAGE_DEV_KIND_PTS_DIR) {
            return 0;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DISK_DIR) {
            if (*cursor) return 0;
            ++*cursor;
            entry->type = RELIEFOS_FS_TYPE_DIR;
            storage_copy_text(entry->name, sizeof(entry->name), "by-partuuid");
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_PARTUUID_DIR) {
            char uuid[37], target[48];
            int step = storage_devlink_next(cursor, uuid, target);
            if (step <= 0) return step;
            entry->type = RELIEFOS_FS_TYPE_SYMLINK;
            storage_copy_text(entry->name, sizeof(entry->name), uuid);
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_SND_DIR) {
            return sound_service_snd_readdir(cursor, entry);
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DRI_DIR) {
            if (!graphics_service_is_registered()) return 0;
            if (*cursor == 0) {
                ++*cursor;
                entry->type = RELIEFOS_FS_TYPE_DEVICE;
                storage_copy_text(entry->name, sizeof(entry->name), "card0");
                return 1;
            }
            if (*cursor == 1 && graphics_service_render_node_ready()) {
                ++*cursor;
                entry->type = RELIEFOS_FS_TYPE_DEVICE;
                storage_copy_text(entry->name, sizeof(entry->name),
                                  "renderD128");
                return 1;
            }
            return 0;
        }
        while (*cursor < count) {
            const struct storage_dev_entry *dev = &entries[*cursor];
            ++(*cursor);
            entry->type = dev->type;
            storage_copy_text(entry->name, sizeof(entry->name), dev->name);
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DIR) {
            /* Expose the Linux block namespace generated from discovered
             * transport identities. */
            uint64_t dynamic = *cursor - count;
            if (graphics_service_is_registered()) {
                if (!dynamic) {
                    ++*cursor;
                    entry->type = RELIEFOS_FS_TYPE_DIR;
                    storage_copy_text(entry->name, sizeof(entry->name), "dri");
                    return 1;
                }
                --dynamic;
            }
            storage_refresh_disks();
            for (uint32_t disk_id = 0; disk_id < g_install_disk_count;
                 ++disk_id) {
                char device_name[RELIEFOS_FS_PATH_LEN];
                uint64_t first_lba;
                uint64_t sectors;
                int block_ret = storage_disk_block_info(disk_id, -1, &first_lba, &sectors);
                if (block_ret < 0) {
                    if (block_ret == -2 || block_ret == -19) continue;
                    return block_ret;
                }
                if (dynamic == 0) {
                    if (storage_disk_device_name(disk_id, -1, device_name,
                                                 sizeof(device_name)) < 0) return -22;
                    ++(*cursor);
                    entry->type = RELIEFOS_FS_TYPE_DEVICE;
                    storage_copy_text(entry->name, sizeof(entry->name), device_name + 5);
                    return 1;
                }
                --dynamic;
                for (uint32_t part = 0; part < RELIEFOS_DISK_MAX_PARTITIONS; ++part) {
                    block_ret = storage_disk_block_info(disk_id, (int32_t)part,
                                                        &first_lba, &sectors);
                    if (block_ret < 0) {
                        if (block_ret == -2 || block_ret == -19) continue;
                        return block_ret;
                    }
                    if (dynamic == 0) {
                        if (storage_disk_device_name(disk_id, (int32_t)part,
                                                     device_name, sizeof(device_name)) < 0)
                            return -22;
                        ++(*cursor);
                        entry->type = RELIEFOS_FS_TYPE_DEVICE;
                        storage_copy_text(entry->name, sizeof(entry->name), device_name + 5);
                        return 1;
                    }
                    --dynamic;
                }
            }
            uint64_t optical_cursor=0;uint32_t optical;int optical_rc;
            while((optical_rc=storage_optical_next(&optical_cursor,&optical))>0) {
                if(!dynamic) {
                    char name[RELIEFOS_FS_PATH_LEN];
                    int rc=storage_disk_device_name(optical,-1,name,sizeof(name));if(rc)return rc;
                    ++*cursor;entry->type=RELIEFOS_FS_TYPE_DEVICE;
                    storage_copy_text(entry->name,sizeof(entry->name),name+5);return 1;
                }
                --dynamic;
            }
            if(optical_rc<0)return optical_rc;
        }
        return 0;
    }
    return -20;
}

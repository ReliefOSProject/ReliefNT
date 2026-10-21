#include "internal.h"



const struct storage_dev_entry *storage_dev_find(const char *name,
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

void storage_dev_node(const struct storage_dev_entry *entry,
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

/**
 * @brief Create a socket or FIFO inode on a supporting filesystem.
 * @param path Resolved, parent-authorized absolute path.
 * @param mode S_IFSOCK or S_IFIFO, without permission bits.
 * @param out Receives the created inode identity.
 * @return Zero or negative errno; unsupported FIFO backends are unchanged.
 */
int storage_create_special(const char *path, uint32_t mode, struct storage_node *out)
{
    if (mode != LINUX_S_IFSOCK && mode != LINUX_S_IFIFO) return -22;
    struct storage_node existing;
    int ret = storage_lookup_path(path, &existing);
    if (!ret) return -17;
    if (ret != -2) return ret;
    struct storage_volume *volume;
    char backend[RELIEFOS_FS_PATH_LEN];
    ret = storage_route_path(path, &volume, backend, sizeof(backend));
    if (ret < 0) return ret;
    if (volume->filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        return tmpfs_create(volume->tmpfs, backend, mode | 0777, NULL, out);
    }
    if (mode == LINUX_S_IFIFO && !storage_ext4_is_ext_family(volume)) return -95;
    ret = storage_write_file(path, "", 0);
    if (ret < 0) return ret;
    ret = storage_lookup_path(path, out);
    if (ret < 0) return ret;
    if (out->flags & STORAGE_NODE_FLAG_EXT2) {
        struct storage_volume *previous = NULL;
        char backend[RELIEFOS_FS_PATH_LEN];
        ret = storage_select_node_volume(out, &previous);
        if (!ret) ret = storage_backend_path(path, backend, sizeof(backend));
        if (!ret) ret = storage_ext4_mark_special(&g_storage, backend, out, mode);
        storage_restore_volume(previous);
    }
    if (ret < 0) { (void)storage_unlink(path); return ret; }
    /* FAT/exFAT retain the directory entry; its type and DAC metadata are
     * persisted in LEONACL.SYS by fs_permissions_create. */
    out->type = mode == LINUX_S_IFSOCK ? RELIEFOS_FS_TYPE_SOCKET : RELIEFOS_FS_TYPE_FIFO;
    return 0;
}

/**
 * @brief Create a filesystem socket inode.
 * @param path Resolved, authorized absolute path.
 * @param out Receives the created inode.
 * @return Zero or negative errno.
 */
int storage_create_socket(const char *path, struct storage_node *out)
{
    return storage_create_special(path, LINUX_S_IFSOCK, out);
}

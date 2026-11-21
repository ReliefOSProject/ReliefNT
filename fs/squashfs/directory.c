/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

struct sqfs_dirent {
    uint64_t ref;
    uint32_t number;
    uint16_t type, length;
    char name[VFS_NAME_MAX + 1];
};

static int dir_begin(struct storage_volume *volume, const struct storage_node *node,
                     struct sqfs_dir_iter *iter)
{
    struct sqfs_inode inode;
    int rc = sqfs_inode_read(volume, node->entry_id, &inode);
    if (rc) return rc;
    if (sqfs_basic_type(inode.disk_type) != 1) return -ENOTDIR;
    *iter = (struct sqfs_dir_iter){
        .position = {volume->squashfs->begin[SQFS_DIRECTORY] + inode.start, inode.offset},
        .bytes_left = (uint32_t)inode.size - 3u}; /* On-disk sizes include the bias for dot entries. */
    return 0;
}

static int dir_read(struct storage_volume *volume, struct sqfs_dir_iter *iter,
                    void *bytes, uint32_t len)
{
    if (len > iter->bytes_left) return -EIO;
    int rc = sqfs_meta_read(volume, SQFS_DIRECTORY, &iter->position, bytes, len);
    if (!rc) iter->bytes_left -= len;
    return rc;
}

static int dir_next(struct storage_volume *volume, struct sqfs_dir_iter *iter,
                    struct sqfs_dirent *entry)
{
    uint8_t field[12];
    if (!iter->bytes_left) return iter->entries_left ? -EIO : 0;
    if (!iter->entries_left) {
        int rc = dir_read(volume, iter, field, 12);
        if (rc) return rc;
        uint32_t count = sqfs_u32(field);
        if (count >= 256 || count + 1 > iter->bytes_left / 9u) return -EIO;
        iter->entries_left = count + 1;
        iter->inode_block = sqfs_u32(field + 4);
        iter->inode_base = sqfs_u32(field + 8);
        struct sqfs_super *fs = volume->squashfs;
        if (iter->inode_block >= fs->end[SQFS_INODE] - fs->begin[SQFS_INODE]) return -EIO;
    }
    int rc = dir_read(volume, iter, field, 8);
    if (rc) return rc;
    unsigned offset = sqfs_u16(field), delta = sqfs_u16(field + 2);
    int64_t number = (int64_t)iter->inode_base + delta - (delta >= 32768 ? 65536 : 0);
    unsigned name_len = (unsigned)sqfs_u16(field + 6) + 1u;
    entry->type = sqfs_u16(field + 4);
    if (offset >= SQFS_META_BYTES || number <= 0 || number > volume->squashfs->inodes ||
        !entry->type || entry->type > 7 || name_len > sizeof(entry->name) - 1) return -EIO;
    rc = dir_read(volume, iter, entry->name, name_len);
    if (rc) return rc;
    for (unsigned i = 0; i < name_len; ++i)
        if (!entry->name[i] || entry->name[i] == '/') return -EIO;
    entry->name[name_len] = 0;
    if (!__builtin_strcmp(entry->name, ".") || !__builtin_strcmp(entry->name, "..")) return -EIO;
    entry->ref = (uint64_t)iter->inode_block << 16 | offset;
    entry->number = (uint32_t)number;
    entry->length = (uint16_t)name_len;
    --iter->entries_left;
    return 1;
}

int sqfs_lookup(struct storage_volume *volume, const struct storage_node *parent,
                const char *name, struct storage_node *out)
{
    struct sqfs_dir_iter iter;
    int rc = dir_begin(volume, parent, &iter);
    if (rc) return rc;
    struct sqfs_dirent entry;
    while ((rc = dir_next(volume, &iter, &entry)) > 0) {
        if (__builtin_strcmp(entry.name, name)) continue;
        struct sqfs_inode inode;
        rc = sqfs_inode_read(volume, entry.ref, &inode);
        if (rc) return rc;
        if (inode.number != entry.number || sqfs_basic_type(inode.disk_type) != entry.type) return -EIO;
        return sqfs_node(volume, entry.ref, &inode, out);
    }
    return rc ? rc : -ENOENT;
}

static int readdir_entry(struct storage_volume *volume, const struct storage_node *node,
                         uint64_t *cookie, char *name, size_t capacity, uint32_t *type)
{
    struct sqfs_super *fs = volume->squashfs;
    if (sqfs_image_changed(volume)) return -EIO;
    struct sqfs_dir_iter iter;
    struct sqfs_dirent entry;
    struct sqfs_dir_cursor *cached = NULL;
    for (unsigned i = 0; i < SQFS_DIR_CURSORS; ++i) {
        if (fs->dir[i].valid && fs->dir[i].inode == node->entry_id && fs->dir[i].cookie == *cookie) {
            cached = &fs->dir[i];
            break;
        }
    }
    if (cached) iter = cached->iter;
    else {
        int rc = dir_begin(volume, node, &iter);
        if (rc) return rc;
        /* The cookie is an ordinal, not a cache slot. A seek or eviction
         * can replay it without resuming from attacker-chosen metadata. */
        if (*cookie > iter.bytes_left / 9u) return 0;
        for (uint64_t i = 0; i < *cookie; ++i) {
            rc = dir_next(volume, &iter, &entry);
            if (rc <= 0) return rc;
        }
        cached = &fs->dir[fs->next_dir++ % SQFS_DIR_CURSORS];
    }
    int rc = dir_next(volume, &iter, &entry);
    if (rc <= 0) return rc;
    if (entry.length >= capacity) return -ENAMETOOLONG;
    if (type) *type = sqfs_node_type(entry.type);
    __builtin_memcpy(name, entry.name, entry.length + 1u);
    *cached = (struct sqfs_dir_cursor){true, node->entry_id, ++*cookie, iter};
    return 1;
}

int sqfs_readdir(struct storage_volume *volume, const struct storage_node *node,
                 uint64_t *cookie, struct reliefos_dir_entry *out, uint64_t flags)
{
    (void)flags;
    return readdir_entry(volume, node, cookie, out->name, sizeof(out->name), &out->type);
}

int sqfs_readdir_name(struct storage_volume *volume, const struct storage_node *node,
                      uint64_t *cookie, char *name, size_t capacity, uint64_t flags)
{
    (void)flags;
    /* SquashFS leaves the two dot entries implicit on disk. Keep their
     * positions in the native stream separate from the stored-entry cursor. */
    if (node->type != RELIEFOS_FS_TYPE_DIR) return -ENOTDIR;
    if (*cookie < 2) {
        if (sqfs_image_changed(volume)) return -EIO;
        const char *dot = *cookie == 0 ? "." : "..";
        size_t length = (size_t)*cookie + 2;
        if (length > capacity) return -ENAMETOOLONG;
        __builtin_memcpy(name, dot, length);
        ++*cookie;
        return 1;
    }
    uint64_t stored_cookie = *cookie - 2;
    int rc = readdir_entry(volume, node, &stored_cookie, name, capacity, NULL);
    if (rc > 0) *cookie = stored_cookie + 2;
    return rc;
}

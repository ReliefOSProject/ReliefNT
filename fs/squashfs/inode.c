/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/mount.h>

uint32_t sqfs_node_type(unsigned type)
{
    switch (sqfs_basic_type(type)) {
    case 1: return RELIEFOS_FS_TYPE_DIR;
    case 2: return RELIEFOS_FS_TYPE_FILE;
    case 3: return RELIEFOS_FS_TYPE_SYMLINK;
    case 4: case 5: return RELIEFOS_FS_TYPE_DEVICE;
    case 6: return RELIEFOS_FS_TYPE_FIFO;
    case 7: return RELIEFOS_FS_TYPE_SOCKET;
    default: return 0;
    }
}

static uint32_t mode_type(unsigned type)
{
    static const uint16_t modes[] = {0, LINUX_S_IFDIR, LINUX_S_IFREG,
        LINUX_S_IFLNK, LINUX_S_IFBLK, LINUX_S_IFCHR, LINUX_S_IFIFO, LINUX_S_IFSOCK};
    return modes[sqfs_basic_type(type)];
}

int sqfs_inode_read(struct storage_volume *volume, uint64_t ref, struct sqfs_inode *out)
{
    static const uint8_t extra_size[] = {0, 16, 16, 8, 8, 8, 4, 4, 24, 40, 8, 12, 12, 8, 8};
    struct sqfs_super *fs = volume->squashfs;
    if (sqfs_image_changed(volume)) return -EIO;
    uint64_t block = ref >> 16;
    uint16_t offset = (uint16_t)ref;
    if (offset >= SQFS_META_BYTES || block >= fs->end[SQFS_INODE] - fs->begin[SQFS_INODE])
        return -EIO;
    struct sqfs_position position = {fs->begin[SQFS_INODE] + block, offset};
    uint8_t field[40];
    int rc = sqfs_meta_read(volume, SQFS_INODE, &position, field, 16);
    if (rc) return rc;
    struct sqfs_inode n = {.disk_type = sqfs_u16(field), .mode = sqfs_u16(field + 2),
        .uid = sqfs_u16(field + 4), .gid = sqfs_u16(field + 6),
        .time = sqfs_u32(field + 8), .number = sqfs_u32(field + 12)};
    if (!n.disk_type || n.disk_type >= sizeof(extra_size) || n.mode > 07777 ||
        n.uid >= fs->id_count || n.gid >= fs->id_count || !n.number || n.number > fs->inodes)
        return -EIO;
    rc = sqfs_meta_read(volume, SQFS_INODE, &position, field, extra_size[n.disk_type]);
    if (rc) return rc;
    uint32_t fragment = UINT32_MAX, xattr = UINT32_MAX;
    switch (n.disk_type) {
    case 1:
        n.start = sqfs_u32(field);
        n.links = sqfs_u32(field + 4);
        n.size = sqfs_u16(field + 8);
        n.offset = sqfs_u16(field + 10);
        break;
    case 8:
        n.links = sqfs_u32(field);
        n.size = sqfs_u32(field + 4);
        n.start = sqfs_u32(field + 8);
        n.offset = sqfs_u16(field + 18);
        xattr = sqfs_u32(field + 20);
        break;
    case 2:
        n.start = sqfs_u32(field);
        fragment = sqfs_u32(field + 4);
        n.size = sqfs_u32(field + 12);
        n.links = 1;
        break;
    case 9:
        n.start = sqfs_u64(field);
        n.size = sqfs_u64(field + 8);
        n.sparse = sqfs_u64(field + 16);
        n.links = sqfs_u32(field + 24);
        fragment = sqfs_u32(field + 28);
        xattr = sqfs_u32(field + 36);
        break;
    case 3: case 10:
        n.links = sqfs_u32(field);
        n.size = sqfs_u32(field + 4);
        if (!n.size || n.size >= VFS_PATH_MAX) return -EIO;
        if (n.disk_type == 10) {
            struct sqfs_position after = position;
            rc = sqfs_meta_read(volume, SQFS_INODE, &after, NULL, (uint32_t)n.size);
            if (!rc) rc = sqfs_meta_read(volume, SQFS_INODE, &after, field, 4);
            if (rc) return rc;
            xattr = sqfs_u32(field);
        }
        break;
    case 4: case 5: case 11: case 12:
        n.links = sqfs_u32(field);
        n.rdev = sqfs_u32(field + 4); /* Linux new_encode_dev format. */
        if (n.disk_type >= 8) xattr = sqfs_u32(field + 8);
        break;
    default: /* FIFO and socket inodes */
        n.links = sqfs_u32(field);
        if (n.disk_type >= 8) xattr = sqfs_u32(field + 4);
        break;
    }
    if (fragment != UINT32_MAX || xattr != UINT32_MAX) return -EOPNOTSUPP;
    if (!n.links || n.size > INT64_MAX || n.sparse > n.size) return -EIO;
    if (sqfs_basic_type(n.disk_type) == 1 &&
        (n.size < 3 || n.offset >= SQFS_META_BYTES || (n.size > 3 &&
          n.start >= fs->end[SQFS_DIRECTORY] - fs->begin[SQFS_DIRECTORY]))) return -EIO;
    if (sqfs_basic_type(n.disk_type) == 2 && n.size && n.start > fs->begin[SQFS_INODE]) return -EIO;
    n.extra = position;
    *out = n;
    return 0;
}

int sqfs_node(struct storage_volume *volume, uint64_t ref, const struct sqfs_inode *n,
              struct storage_node *out)
{
    *out = (struct storage_node){.type = sqfs_node_type(n->disk_type),
        .flags = STORAGE_NODE_FLAG_SQUASHFS, .first_cluster = n->number,
        .volume_id = volume->volume_id, .size = n->size, .entry_id = ref};
    if (ref == volume->squashfs->root) out->flags |= STORAGE_NODE_FLAG_ROOT;
    if (out->type == RELIEFOS_FS_TYPE_DEVICE)
        out->flags |= STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_RDEV;
    return 0;
}

int sqfs_readlink(struct storage_volume *volume, const struct storage_node *node,
                  char *bytes, uint32_t capacity, uint32_t *length)
{
    struct sqfs_inode n;
    int rc = sqfs_inode_read(volume, node->entry_id, &n);
    if (rc) return rc;
    if (sqfs_basic_type(n.disk_type) != 3) return -EINVAL;
    if (n.size > capacity) return -ENAMETOOLONG;
    rc = sqfs_meta_read(volume, SQFS_INODE, &n.extra, bytes, (uint32_t)n.size);
    if (rc) return rc;
    for (uint32_t i = 0; i < n.size; ++i) if (!bytes[i]) return -EIO;
    *length = (uint32_t)n.size;
    return 0;
}

static int owner(struct storage_volume *volume, const struct sqfs_inode *n, uint32_t *uid, uint32_t *gid)
{
    int rc = sqfs_id(volume, n->uid, uid);
    if (rc) return rc;
    if (n->uid == n->gid) { *gid = *uid; return 0; }
    return sqfs_id(volume, n->gid, gid);
}

int sqfs_stat(struct storage_volume *volume, const struct storage_node *node, struct linux_stat_abi *out)
{
    struct sqfs_inode n;
    int rc = sqfs_inode_read(volume, node->entry_id, &n);
    if (rc) return rc;
    *out = (struct linux_stat_abi){.st_dev = storage_volume_device_number(volume),
        .st_ino = n.number, .st_nlink = n.links, .st_mode = mode_type(n.disk_type) | n.mode,
        .st_size = (int64_t)n.size, .st_rdev = n.rdev, .st_blksize = volume->squashfs->block_size,
        .st_blocks = (int64_t)((n.size - n.sparse + 511u) / 512u),
        .atime_sec = n.time, .mtime_sec = n.time, .ctime_sec = n.time};
    return owner(volume, &n, &out->st_uid, &out->st_gid);
}

int sqfs_permissions(struct storage_volume *volume, const struct storage_node *node,
                     struct reliefos_permissions *out, bool write)
{
    if (write) return -EROFS;
    struct sqfs_inode n;
    int rc = sqfs_inode_read(volume, node->entry_id, &n);
    if (rc) return rc;
    out->mode = n.mode;
    return owner(volume, &n, &out->uid, &out->gid);
}

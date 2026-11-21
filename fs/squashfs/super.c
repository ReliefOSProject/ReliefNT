/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/mount.h>

#define SQFS_GZIP 1u
#define SQFS_COMP_OPTIONS 0x0400u

static void sqfs_destroy(struct storage_volume *volume)
{
    struct sqfs_super *fs = volume->squashfs;
    if (!fs) return;
    volume->squashfs = NULL;
    kernel_free(fs->input);
    kernel_free(fs->data);
    kernel_free(fs);
}

static int sqfs_root(struct storage_volume *volume, struct storage_node *out)
{
    struct sqfs_inode inode;
    uint64_t ref = volume->squashfs->root;
    int rc = sqfs_inode_read(volume, ref, &inode);
    if (rc) return rc;
    if (sqfs_basic_type(inode.disk_type) != 1) return -EIO;
    return sqfs_node(volume, ref, &inode, out);
}

static int parse_tables(struct storage_volume *volume, const uint8_t *header)
{
    struct sqfs_super *fs = volume->squashfs;
    uint64_t ids = sqfs_u64(header + 48), id_blocks = (fs->id_count + SQFS_IDS_PER_BLOCK - 1) /
        SQFS_IDS_PER_BLOCK;
    uint64_t inode = sqfs_u64(header + 64), dir = sqfs_u64(header + 72);
    uint64_t fragment = sqfs_u64(header + 80), exports = sqfs_u64(header + 88);
    if (inode < fs->data_start || dir <= inode || ids <= dir ||
        !sqfs_range(ids, id_blocks * 8u, fs->bytes_used)) return -EINVAL;

    uint8_t locations[SQFS_ID_BLOCKS * 8u];
    int rc = sqfs_read_at(volume, ids, locations, (uint32_t)(id_blocks * 8u), fs->bytes_used);
    if (rc) return rc;
    for (unsigned i = 0; i < id_blocks; ++i) {
        uint64_t block = sqfs_u64(locations + i * 8u);
        if (block < dir || block >= ids || (i && block <= fs->id_blocks[i - 1])) return -EINVAL;
        fs->id_blocks[i] = block;
    }

    uint64_t dir_end = fs->id_blocks[0];
    if (exports != UINT64_MAX) {
        /* Export records are not needed for local lookups, but their metadata
         * follows the directory stream and must not be parsed as directory data. */
        uint64_t blocks = ((uint64_t)fs->inodes + 1023u) / 1024u;
        if (exports <= dir || !sqfs_range(exports, blocks * 8u, dir_end)) return -EINVAL;
        uint8_t first[8];
        rc = sqfs_read_at(volume, exports, first, sizeof(first), dir_end);
        if (rc) return rc;
        dir_end = sqfs_u64(first);
        if (dir_end < dir || dir_end >= exports) return -EINVAL;
    }
    /* squashfs-tools can record the current output position for an empty
     * fragment table. No fragment index or metadata is present in that case. */
    if (fragment != UINT64_MAX) {
        if (fragment < dir || fragment > dir_end) return -EINVAL;
        dir_end = fragment;
    }
    fs->begin[SQFS_INODE] = inode;
    fs->end[SQFS_INODE] = fs->begin[SQFS_DIRECTORY] = dir;
    fs->end[SQFS_DIRECTORY] = dir_end;
    fs->begin[SQFS_ID] = fs->id_blocks[0];
    fs->end[SQFS_ID] = ids;
    return 0;
}

static int sqfs_mount(struct storage_volume *volume)
{
    if (!(volume->mount_flags & MS_RDONLY)) return -EROFS;
    if (volume->squashfs) return -EBUSY;
    if (volume->ext_sector_count > UINT64_MAX / SECTOR_SIZE) return -EINVAL;
    uint64_t device_bytes = volume->ext_sector_count * SECTOR_SIZE;
    if (device_bytes < SQFS_SUPER_BYTES) return -EINVAL;

    uint8_t header[SQFS_SUPER_BYTES];
    int rc = sqfs_read_at(volume, 0, header, sizeof(header), device_bytes);
    if (rc) return rc;
    if (sqfs_u32(header) != SQFS_MAGIC) return -EINVAL;
    if (sqfs_u16(header + 28) != 4 || sqfs_u16(header + 30)) return -EOPNOTSUPP;
    if (sqfs_u16(header + 20) != SQFS_GZIP || sqfs_u32(header + 16) ||
        sqfs_u64(header + 56) != UINT64_MAX) return -EOPNOTSUPP;
    unsigned log = sqfs_u16(header + 22), id_count = sqfs_u16(header + 26);
    uint32_t block_size = sqfs_u32(header + 12), inodes = sqfs_u32(header + 4);
    uint64_t bytes_used = sqfs_u64(header + 40), data_start = SQFS_SUPER_BYTES;
    if (log < 12 || log > 20 || block_size != (1u << log) || !id_count || !inodes ||
        bytes_used <= SQFS_SUPER_BYTES || bytes_used > device_bytes) return -EINVAL;
    if (sqfs_u16(header + 24) & SQFS_COMP_OPTIONS) {
        uint8_t options[10];
        rc = sqfs_read_at(volume, SQFS_SUPER_BYTES, options, sizeof(options), bytes_used);
        if (rc) return rc;
        unsigned level = sqfs_u32(options + 2), window = sqfs_u16(options + 6);
        if (sqfs_u16(options) != 0x8008u || level < 1 || level > 9 || window < 8 || window > 15 ||
            (sqfs_u16(options + 8) & ~0x1fu)) return -EINVAL;
        data_start += sizeof(options);
    }

    struct sqfs_super *fs = kernel_malloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    __builtin_memset(fs, 0, sizeof(*fs));
    volume->squashfs = fs;
    fs->block_size = block_size;
    fs->inodes = inodes;
    fs->id_count = id_count;
    fs->bytes_used = bytes_used;
    fs->data_start = data_start;
    fs->root = sqfs_u64(header + 32);
    fs->device_generation = block_device_lookup_generation(&volume->device_ref);
    rc = parse_tables(volume, header);
    if (rc) goto failed;
    fs->input = kernel_malloc(block_size < SQFS_META_BYTES ? SQFS_META_BYTES : block_size);
    fs->data = kernel_malloc(block_size);
    if (!fs->input || !fs->data) { rc = -ENOMEM; goto failed; }
    struct storage_node root;
    rc = sqfs_root(volume, &root);
    if (rc) goto failed;
    volume->filesystem = STORAGE_FILESYSTEM_SQUASHFS;
    return 0;

failed:
    sqfs_destroy(volume);
    return rc;
}

static int sqfs_statfs(struct storage_volume *volume, struct linux_statfs_abi *value)
{
    if (sqfs_image_changed(volume)) return -EIO;
    struct sqfs_super *fs = volume->squashfs;
    value->f_type = SQFS_MAGIC;
    value->f_bsize = value->f_frsize = fs->block_size;
    value->f_blocks = (fs->bytes_used - 1) / fs->block_size + 1;
    value->f_files = fs->inodes;
    value->f_namelen = RELIEFOS_FS_NAME_LEN - 1;
    value->f_flags |= LINUX_ST_RDONLY;
    return 0;
}

const struct storage_filesystem_ops storage_squashfs_ops = {
    .name = "squashfs",
    .flags = STORAGE_FS_READONLY | STORAGE_FS_LOOKUP_EPOCH,
    .mount = sqfs_mount,
    .destroy = sqfs_destroy,
    .root = sqfs_root,
    .lookup = sqfs_lookup,
    .readdir = sqfs_readdir,
    .read = sqfs_file_read,
    .readlink = sqfs_readlink,
    .stat = sqfs_stat,
    .permissions = sqfs_permissions,
    .statfs = sqfs_statfs,
};

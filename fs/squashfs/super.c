/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "codecs/interface.h"
#include <reliefnt/mount.h>

#define SQFS_COMP_OPTIONS 0x0400u

static void sqfs_destroy(struct storage_volume *volume)
{
    struct sqfs_super *fs = volume->squashfs;
    if (!fs) return;
    volume->squashfs = NULL;
    sqfs_codec_destroy(fs->compression, fs->codec);
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

/* Index pointers are on disk after their metadata. The first pointer marks
 * the end of the preceding table. Individual pointers are read on demand. */
static int index_bounds(struct storage_volume *volume, uint64_t index, uint32_t count,
                        uint32_t width, uint64_t floor, uint64_t ceiling, uint64_t *first)
{
    uint64_t pointers = ((uint64_t)count * width + SQFS_META_BYTES - 1) / SQFS_META_BYTES;
    if (!count || index <= floor || !sqfs_range(index, pointers * 8, ceiling)) return -EINVAL;
    uint8_t record[8];
    int rc = sqfs_read_at(volume, index, record, sizeof(record), ceiling);
    if (rc) return rc;
    *first = sqfs_u64(record);
    return *first >= floor && *first < index ? 0 : -EINVAL;
}

static int parse_index(struct storage_volume *volume, enum sqfs_table table, uint64_t index,
                       uint32_t count, uint32_t width, uint64_t floor, uint64_t ceiling)
{
    struct sqfs_super *fs = volume->squashfs;
    int rc = index_bounds(volume, index, count, width, floor, ceiling, &fs->begin[table]);
    if (rc) return rc;
    fs->end[table] = index;
    fs->index[table] = (struct sqfs_index){index, count, width};
    return 0;
}

static int parse_tables(struct storage_volume *volume, const uint8_t *header)
{
    struct sqfs_super *fs = volume->squashfs;
    uint64_t ids = sqfs_u64(header + 48), xattrs = sqfs_u64(header + 56);
    uint64_t inode = sqfs_u64(header + 64), dir = sqfs_u64(header + 72);
    uint64_t fragment = sqfs_u64(header + 80), exports = sqfs_u64(header + 88);
    if (inode < fs->data_start || dir <= inode || ids <= dir) return -EINVAL;

    uint64_t next = fs->bytes_used;
    if (xattrs != UINT64_MAX) {
        uint8_t geometry[16];
        if (xattrs <= ids || !sqfs_range(xattrs, sizeof(geometry), next)) return -EINVAL;
        int rc = sqfs_read_at(volume, xattrs, geometry, sizeof(geometry), next);
        if (rc) return rc;
        uint64_t values = sqfs_u64(geometry);
        if (values <= ids || values >= xattrs) return -EINVAL;
        rc = parse_index(volume, SQFS_XATTR_ID, xattrs + sizeof(geometry), sqfs_u32(geometry + 8),
                         16, values, next);
        if (rc) return rc;
        fs->end[SQFS_XATTR_ID] = xattrs;
        fs->begin[SQFS_XATTR] = values;
        fs->end[SQFS_XATTR] = fs->begin[SQFS_XATTR_ID];
        if (values >= fs->end[SQFS_XATTR]) return -EINVAL;
        next = values;
    }
    int rc = parse_index(volume, SQFS_ID, ids, sqfs_u16(header + 26), 4, dir, next);
    if (rc) return rc;
    next = fs->begin[SQFS_ID];
    if (exports != UINT64_MAX) {
        rc = index_bounds(volume, exports, fs->inodes, 8, dir, next, &next);
        if (rc) return rc;
    }
    uint32_t fragments = sqfs_u32(header + 16);
    if (fragments) {
        rc = parse_index(volume, SQFS_FRAGMENT, fragment, fragments, 16, dir, next);
        if (rc) return rc;
        next = fs->begin[SQFS_FRAGMENT];
    } else if (fragment != UINT64_MAX) {
        /* squashfs-tools also records the position of an empty index. */
        if (fragment < dir || fragment > next) return -EINVAL;
        next = fragment;
    }
    fs->begin[SQFS_INODE] = inode;
    fs->end[SQFS_INODE] = fs->begin[SQFS_DIRECTORY] = dir;
    fs->end[SQFS_DIRECTORY] = next;
    return 0;
}

int sqfs_decompress(struct sqfs_super *fs, const uint8_t *src, uint32_t stored,
                    uint8_t *dst, uint32_t capacity, uint32_t *size)
{
    if (fs->compression == SQFS_GZIP) return sqfs_inflate(src, stored, dst, capacity, size);
    return sqfs_codec_decode(fs->compression, fs->codec, src, stored, dst, capacity, size);
}

static int sqfs_setup_codec(struct storage_volume *volume, uint16_t flags)
{
    struct sqfs_super *fs = volume->squashfs;
    uint8_t options[12];
    uint16_t size = 0;
    if (flags & SQFS_COMP_OPTIONS) {
        uint8_t metadata[2];
        int rc = sqfs_read_at(volume, fs->data_start, metadata, sizeof(metadata), fs->bytes_used);
        if (rc) return rc;
        uint16_t descriptor = sqfs_u16(metadata);
        size = descriptor & 0x7fffu;
        if (!(descriptor & 0x8000u) || !size || size > sizeof(options)) return -EINVAL;
        fs->data_start += sizeof(metadata);
        rc = sqfs_read_at(volume, fs->data_start, options, size, fs->bytes_used);
        if (rc) return rc;
        fs->data_start += size;
    }
    if (fs->compression == SQFS_GZIP) {
        if (!size) return 0;
        if (size != 8 || sqfs_u32(options) < 1 || sqfs_u32(options) > 9 ||
            sqfs_u16(options + 4) < 8 || sqfs_u16(options + 4) > 15 ||
            (sqfs_u16(options + 6) & ~0x1fu)) return -EINVAL;
        return 0;
    }
    return sqfs_codec_init(fs->compression, fs->block_size, size ? options : NULL, size,
                           &fs->codec);
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
    unsigned log = sqfs_u16(header + 22), id_count = sqfs_u16(header + 26);
    uint32_t block_size = sqfs_u32(header + 12), inodes = sqfs_u32(header + 4);
    uint64_t bytes_used = sqfs_u64(header + 40);
    if (log < 12 || log > 20 || block_size != (1u << log) || !id_count || !inodes ||
        bytes_used <= SQFS_SUPER_BYTES || bytes_used > device_bytes) return -EINVAL;
    struct sqfs_super *fs = kernel_malloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    __builtin_memset(fs, 0, sizeof(*fs));
    volume->squashfs = fs;
    fs->block_size = block_size;
    fs->inodes = inodes;
    fs->compression = sqfs_u16(header + 20);
    fs->bytes_used = bytes_used;
    fs->data_start = SQFS_SUPER_BYTES;
    fs->root = sqfs_u64(header + 32);
    fs->device_generation = block_device_lookup_generation(&volume->device_ref);
    rc = sqfs_setup_codec(volume, sqfs_u16(header + 24));
    if (!rc) rc = parse_tables(volume, header);
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
    value->f_namelen = VFS_NAME_MAX;
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
    .readdir_name = sqfs_readdir_name,
    .read = sqfs_file_read,
    .readlink = sqfs_readlink,
    .stat = sqfs_stat,
    .permissions = sqfs_permissions,
    .statfs = sqfs_statfs,
    .getxattr = sqfs_getxattr,
    .listxattr = sqfs_listxattr,
};

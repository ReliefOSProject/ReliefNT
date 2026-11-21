/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Consume one file block descriptor and advance the absolute data position.
 * A zero descriptor denotes a sparse hole and consumes no data bytes. */
static int next_block(struct storage_volume *volume, struct sqfs_file_cursor *pos,
                      uint32_t *descriptor)
{
    struct sqfs_super *fs = volume->squashfs;
    uint8_t field[4];
    int rc = sqfs_meta_read(volume, SQFS_INODE, &pos->list, field, sizeof(field));
    if (rc) return rc;
    uint32_t value = sqfs_u32(field), bytes = value & (SQFS_DATA_RAW - 1u);
    if (value >= 2u * SQFS_DATA_RAW || (value && !bytes) || bytes > fs->block_size ||
        (bytes && (pos->disk < fs->data_start || !sqfs_range(pos->disk, bytes, fs->begin[SQFS_INODE]))))
        return -EIO;
    *descriptor = value;
    pos->disk += bytes;
    ++pos->index;
    return 0;
}

static int data_block(struct storage_volume *volume, uint64_t disk, uint32_t descriptor,
                      uint32_t expected)
{
    struct sqfs_super *fs = volume->squashfs;
    if (fs->data_descriptor == descriptor && fs->data_block == disk)
        return !expected || fs->data_size == expected ? 0 : -EIO;
    fs->data_descriptor = 0;
    bool raw = (descriptor & SQFS_DATA_RAW) != 0;
    uint32_t stored = descriptor & (SQFS_DATA_RAW - 1u);
    if (descriptor >= 2u * SQFS_DATA_RAW || !stored || stored > fs->block_size ||
        disk < fs->data_start || (raw && expected && stored != expected)) return -EIO;
    int rc = sqfs_read_at(volume, disk, raw ? fs->data : fs->input, stored, fs->begin[SQFS_INODE]);
    if (rc) return rc;
    uint32_t size = stored;
    if (!raw) {
        rc = sqfs_decompress(fs, fs->input, stored, fs->data, expected ? expected : fs->block_size,
                             &size);
        if (rc) return rc;
        if (!size || (expected && size != expected)) return -EIO;
    }
    fs->data_block = disk;
    fs->data_descriptor = descriptor;
    fs->data_size = size;
    return 0;
}

static int read_fragment(struct storage_volume *volume, const struct sqfs_inode *inode,
                         uint32_t skip, void *bytes, uint32_t length)
{
    struct sqfs_super *fs = volume->squashfs;
    uint8_t field[16];
    int rc = sqfs_index_read(volume, SQFS_FRAGMENT, inode->fragment, field);
    if (rc) return rc;
    rc = data_block(volume, sqfs_u64(field), sqfs_u32(field + 8), 0);
    if (rc) return rc;
    uint32_t offset = inode->fragment_offset + skip;
    if (!sqfs_range(offset, length, fs->data_size)) return -EIO;
    __builtin_memcpy(bytes, fs->data + offset, length);
    return 0;
}

int sqfs_file_read(struct storage_volume *volume, const struct storage_node *node, uint64_t offset,
                   void *bytes, uint32_t capacity, uint32_t *length,
                   struct storage_read_cursor *cursor, uint64_t flags)
{
    (void)flags;
    if (length) *length = 0;
    if (cursor) cursor->valid = 0;
    struct sqfs_inode inode;
    int rc = sqfs_inode_read(volume, node->entry_id, &inode);
    if (rc) return rc;
    if (sqfs_basic_type(inode.disk_type) != 2) return -EINVAL;
    if (!capacity || offset >= inode.size) return 0;
    if (capacity > inode.size - offset) capacity = (uint32_t)(inode.size - offset);

    struct sqfs_super *fs = volume->squashfs;
    uint64_t block = offset / fs->block_size;
    uint64_t fragment_block = inode.fragment == SQFS_INVALID_INDEX ? UINT64_MAX :
        inode.size / fs->block_size;
    uint32_t skip = offset % fs->block_size;
    if (block == fragment_block) {
        rc = read_fragment(volume, &inode, skip, bytes, capacity);
        if (!rc && length) *length = capacity;
        return rc;
    }
    struct sqfs_file_cursor pos = fs->file;
    if (!pos.valid || pos.inode != node->entry_id || pos.index > block)
        pos = (struct sqfs_file_cursor){.valid = true, .inode = node->entry_id,
            .disk = inode.start, .list = inode.extra};
    uint32_t descriptor;
    while (pos.index < block) {
        rc = next_block(volume, &pos, &descriptor);
        if (rc) return rc;
    }
    uint8_t *out = bytes;
    uint32_t done = 0;
    while (done < capacity && pos.index < fragment_block) {
        uint64_t disk = pos.disk, available = inode.size - pos.index * fs->block_size;
        uint32_t expected = available < fs->block_size ? (uint32_t)available : fs->block_size;
        fs->file = pos;
        rc = next_block(volume, &pos, &descriptor);
        if (rc) return rc;
        uint32_t take = expected - skip;
        if (take > capacity - done) take = capacity - done;
        if (!descriptor) __builtin_memset(out + done, 0, take);
        else {
            rc = data_block(volume, disk, descriptor, expected);
            if (rc) return rc;
            __builtin_memcpy(out + done, fs->data + skip, take);
        }
        done += take;
        if (take == expected - skip) fs->file = pos;
        skip = 0;
    }
    if (done < capacity) {
        rc = read_fragment(volume, &inode, 0, out + done, capacity - done);
        if (rc) return rc;
    }
    if (length) *length = capacity;
    return 0;
}

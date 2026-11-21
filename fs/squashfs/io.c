/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/storage_context.h>

/* An unaligned extent can take several transport commands. Do not let any
 * of them escape to syscall replay, which would start again at the first LBA. */
int sqfs_read_at(struct storage_volume *volume, uint64_t offset, void *output,
                 uint32_t length, uint64_t limit)
{
    if (!sqfs_range(offset, length, limit)) return -EIO;
    uint8_t *out = output;
    uint8_t tail[SECTOR_SIZE];
    int rc = 0;
    struct storage_io_context saved;
    storage_kernel_context_enter(&saved);
    while (length) {
        uint64_t sector = volume->ext_start_lba + offset / SECTOR_SIZE;
        unsigned skip = offset % SECTOR_SIZE;
        uint32_t count;
        if (skip || length < SECTOR_SIZE) {
            count = SECTOR_SIZE - skip;
            if (count > length) count = length;
            rc = storage_read_device(volume, sector, 1, tail);
            if (rc) break;
            __builtin_memcpy(out, tail + skip, count);
        } else {
            uint32_t sectors = length / SECTOR_SIZE;
            rc = storage_read_device(volume, sector, sectors, out);
            if (rc) break;
            count = sectors * SECTOR_SIZE;
        }
        out += count;
        offset += count;
        length -= count;
    }
    storage_kernel_context_leave(&saved);
    return rc;
}

static int metadata(struct storage_volume *volume, enum sqfs_table table, uint64_t block)
{
    struct sqfs_super *fs = volume->squashfs;
    struct sqfs_meta_cache *cache = &fs->meta[table];
    if (cache->valid && cache->block == block) return 0;
    cache->valid = false;
    if (block < fs->begin[table]) return -EIO;
    uint8_t header[2];
    int rc = sqfs_read_at(volume, block, header, sizeof(header), fs->end[table]);
    if (rc) return rc;
    unsigned descriptor = sqfs_u16(header), size = descriptor & 0x7fffu;
    if (!size || size > SQFS_META_BYTES) return -EIO;
    bool raw = (descriptor & 0x8000u) != 0;
    rc = sqfs_read_at(volume, block + sizeof(header), raw ? cache->data : fs->input,
                       size, fs->end[table]);
    if (rc) return rc;
    uint32_t length = size;
    if (!raw) {
        rc = sqfs_decompress(fs, fs->input, size, cache->data, SQFS_META_BYTES, &length);
        if (rc) return rc;
        if (!length) return -EIO;
    }
    cache->block = block;
    cache->next = block + sizeof(header) + size;
    cache->length = length;
    cache->valid = true;
    return 0;
}

int sqfs_meta_read(struct storage_volume *volume, enum sqfs_table table,
                   struct sqfs_position *position, void *output, uint32_t length)
{
    uint8_t *out = output;
    struct sqfs_meta_cache *cache = &volume->squashfs->meta[table];
    while (length) {
        int rc = metadata(volume, table, position->block);
        if (rc) return rc;
        if (position->offset >= cache->length) return -EIO;
        uint32_t count = cache->length - position->offset;
        if (count > length) count = length;
        if (out) {
            __builtin_memcpy(out, cache->data + position->offset, count);
            out += count;
        }
        position->offset += count;
        if (position->offset == cache->length) {
            position->block = cache->next;
            position->offset = 0;
        }
        length -= count;
    }
    return 0;
}

/* Keep the raw index bounded regardless of how many fragments or xattrs exist. */
int sqfs_index_read(struct storage_volume *volume, enum sqfs_table table,
                    uint32_t index, void *record)
{
    struct sqfs_super *fs = volume->squashfs;
    const struct sqfs_index *layout = &fs->index[table];
    if (index >= layout->entries) return -EIO;
    uint64_t entry = (uint64_t)index * layout->record_size;
    uint64_t pointer = entry / SQFS_META_BYTES * 8;
    uint64_t page = pointer / sizeof(fs->index_cache) * sizeof(fs->index_cache);
    uint64_t disk = layout->disk + page;
    if (!fs->index_cache_size || fs->index_cache_disk != disk) {
        uint64_t bytes = ((uint64_t)layout->entries * layout->record_size + SQFS_META_BYTES - 1)
            / SQFS_META_BYTES * 8;
        uint32_t count = bytes - page < sizeof(fs->index_cache) ? (uint32_t)(bytes - page) :
            sizeof(fs->index_cache);
        fs->index_cache_size = 0;
        int rc = sqfs_read_at(volume, disk, fs->index_cache, count, fs->bytes_used);
        if (rc) return rc;
        fs->index_cache_disk = disk;
        fs->index_cache_size = count;
    }
    struct sqfs_position pos = {sqfs_u64(fs->index_cache + pointer - page), entry % SQFS_META_BYTES};
    /* Fixed records never straddle a metadata block. */
    int rc = metadata(volume, table, pos.block);
    if (rc) return rc;
    const struct sqfs_meta_cache *cache = &fs->meta[table];
    if (!sqfs_range(pos.offset, layout->record_size, cache->length)) return -EIO;
    __builtin_memcpy(record, cache->data + pos.offset, layout->record_size);
    return 0;
}

int sqfs_id(struct storage_volume *volume, uint16_t index, uint32_t *id)
{
    uint8_t field[4];
    int rc = sqfs_index_read(volume, SQFS_ID, index, field);
    if (!rc) *id = sqfs_u32(field);
    return rc;
}

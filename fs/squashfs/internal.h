/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SQUASHFS_INTERNAL_H
#define RELIEFNT_SQUASHFS_INTERNAL_H

#include "../../block/core/storage_internal.h"

#define SQFS_MAGIC 0x73717368u
#define SQFS_SUPER_BYTES 96u
#define SQFS_META_BYTES 8192u
#define SQFS_IDS_PER_BLOCK (SQFS_META_BYTES / 4u)
#define SQFS_ID_BLOCKS (UINT16_MAX / SQFS_IDS_PER_BLOCK + 1u)
#define SQFS_DATA_RAW (1u << 24)
#define SQFS_DIR_CURSORS 4u

enum sqfs_table { SQFS_INODE, SQFS_DIRECTORY, SQFS_ID, SQFS_TABLES };

struct sqfs_position {
    uint64_t block; /* Absolute image byte offset of the metadata header. */
    uint32_t offset;
};

struct sqfs_meta_cache {
    uint64_t block, next;
    uint32_t length;
    bool valid;
    uint8_t data[SQFS_META_BYTES];
};

struct sqfs_dir_iter {
    struct sqfs_position position;
    uint32_t bytes_left, entries_left, inode_block, inode_base;
};

struct sqfs_dir_cursor {
    bool valid;
    uint64_t inode, cookie;
    struct sqfs_dir_iter iter;
};

struct sqfs_file_cursor {
    bool valid;
    uint64_t inode, index, disk;
    struct sqfs_position list;
};

struct sqfs_super {
    uint64_t bytes_used, data_start, root;
    uint64_t device_generation;
    uint64_t begin[SQFS_TABLES], end[SQFS_TABLES];
    uint64_t id_blocks[SQFS_ID_BLOCKS];
    uint32_t inodes, id_count, block_size;
    struct sqfs_meta_cache meta[SQFS_TABLES];
    struct sqfs_dir_cursor dir[SQFS_DIR_CURSORS];
    unsigned next_dir;
    struct sqfs_file_cursor file;
    uint64_t data_block;
    uint32_t data_descriptor, data_size;
    uint8_t *input, *data;
};

/* Cached geometry is authoritative only while the backing is unmodified. */
static inline bool sqfs_image_changed(const struct storage_volume *volume)
{
    return block_device_lookup_generation(&volume->device_ref) != volume->squashfs->device_generation;
}

struct sqfs_inode {
    struct sqfs_position extra;
    uint64_t size, start, sparse;
    uint32_t number, time, links, rdev;
    uint16_t disk_type, mode, uid, gid, offset;
};

static inline uint16_t sqfs_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t sqfs_u32(const uint8_t *p)
{
    return (uint32_t)sqfs_u16(p) | (uint32_t)sqfs_u16(p + 2) << 16;
}

static inline uint64_t sqfs_u64(const uint8_t *p)
{
    return (uint64_t)sqfs_u32(p) | (uint64_t)sqfs_u32(p + 4) << 32;
}

static inline bool sqfs_range(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static inline unsigned sqfs_basic_type(unsigned type)
{
    return type >= 8 ? type - 7 : type;
}

/* All codec calls run under storage execution ownership; the fixed-size
 * caches and scratch memory belong to the mounted volume, not global state. */
int sqfs_read_at(struct storage_volume *, uint64_t, void *, uint32_t, uint64_t);
int sqfs_meta_read(struct storage_volume *, enum sqfs_table, struct sqfs_position *, void *, uint32_t);
int sqfs_id(struct storage_volume *, uint16_t, uint32_t *);
int sqfs_inflate(const uint8_t *, size_t, uint8_t *, size_t, uint32_t *);
int sqfs_inode_read(struct storage_volume *, uint64_t, struct sqfs_inode *);
int sqfs_node(struct storage_volume *, uint64_t, const struct sqfs_inode *, struct storage_node *);
uint32_t sqfs_node_type(unsigned);
int sqfs_lookup(struct storage_volume *, const struct storage_node *, const char *, struct storage_node *);
int sqfs_readdir(struct storage_volume *, const struct storage_node *, uint64_t *, struct reliefos_dir_entry *, uint64_t);
int sqfs_file_read(struct storage_volume *, const struct storage_node *, uint64_t, void *, uint32_t,
                   uint32_t *, struct storage_read_cursor *, uint64_t);
int sqfs_readlink(struct storage_volume *, const struct storage_node *, char *, uint32_t, uint32_t *);
int sqfs_stat(struct storage_volume *, const struct storage_node *, struct linux_stat_abi *);
int sqfs_permissions(struct storage_volume *, const struct storage_node *, struct reliefos_permissions *, bool);
extern const struct storage_filesystem_ops storage_squashfs_ops;
#endif

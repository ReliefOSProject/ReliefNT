/* ext4 on-disk format layer: little-endian decoding, bounds checks and
 * pure parsers for the superblock, group descriptors and extent headers.
 * Host-testable: no kernel services, no globals, no I/O.  On error the
 * output views are left unspecified and a negative RELIEFOS_* errno is
 * returned (-RELIEFOS_EINVAL for format violations, -RELIEFOS_EOVERFLOW
 * for offset+size arithmetic overflow). */
#include "../../block/core/storage_internal.h"

/* Field offsets in the little-endian on-disk structs (linux/fs/ext4/
 * ext4.h and ext4_extents.h, Linux v7.3-rc5 reference tree). */
#define SB_BLOCKS_COUNT_LO 0x04
#define SB_R_BLOCKS_COUNT_LO 0x08
#define SB_FREE_BLOCKS_COUNT_LO 0x0C
#define SB_FREE_INODES_COUNT 0x10
#define SB_FIRST_DATA_BLOCK 0x14
#define SB_LOG_BLOCK_SIZE 0x18
#define SB_LOG_CLUSTER_SIZE 0x1C
#define SB_BLOCKS_PER_GROUP 0x20
#define SB_INODES_PER_GROUP 0x28
#define SB_MAGIC 0x38
#define SB_REV_LEVEL 0x4C
#define SB_INODE_SIZE 0x58
#define SB_FEATURE_INCOMPAT 0x60
#define SB_FEATURE_RO_COMPAT 0x64
#define SB_UUID 0x68
#define SB_VOLUME_NAME 0x78
#define SB_JOURNAL_INUM 0xE0
#define SB_DESC_SIZE 0xFE
#define SB_BLOCKS_COUNT_HI 0x150
#define SB_R_BLOCKS_COUNT_HI 0x154
#define SB_FREE_BLOCKS_COUNT_HI 0x158

#define SB_INODES_COUNT 0x00
#define SB_FEATURE_COMPAT 0x5C

#define GD_BLOCK_BITMAP_LO 0x00
#define GD_INODE_BITMAP_LO 0x04
#define GD_INODE_TABLE_LO 0x08
#define GD_FREE_BLOCKS_LO 0x0C
#define GD_FREE_INODES_LO 0x0E
#define GD_USED_DIRS_LO 0x10
#define GD_FLAGS 0x12
#define GD_ITABLE_UNUSED_LO 0x1C
#define GD_CHECKSUM 0x1E
#define GD_BLOCK_BITMAP_HI 0x20
#define GD_INODE_BITMAP_HI 0x24
#define GD_INODE_TABLE_HI 0x28
#define GD_FREE_BLOCKS_HI 0x2C
#define GD_FREE_INODES_HI 0x2E
#define GD_USED_DIRS_HI 0x30
#define GD_ITABLE_UNUSED_HI 0x32

#define EH_MAGIC 0x00
#define EH_ENTRIES 0x02
#define EH_MAX 0x04
#define EH_DEPTH 0x06
#define EH_GENERATION 0x08
#define EH_SIZE 12u

/* 64-bit descriptor words are only meaningful from 64 bytes on (the
 * classic descriptor is 32 bytes; ext4_group_desc hi words follow). */
#define GD_HI_MIN_DESC_SIZE 64u

/* ext4.h: EXT4_MAX_CLUSTER_LOG_SIZE(28) - EXT4_MIN_BLOCK_LOG_SIZE(10).
 * Bounds s_log_cluster_size before the shift below. */
#define MAX_LOG_CLUSTER_SIZE 18u

uint16_t ext4_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t ext4_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t ext4_get_le64(const uint8_t *p)
{
    return (uint64_t)ext4_get_le32(p) |
           ((uint64_t)ext4_get_le32(p + 4) << 32);
}

void ext4_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

void ext4_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

void ext4_put_le64(uint8_t *p, uint64_t value)
{
    ext4_put_le32(p, (uint32_t)(value & 0xffffffffu));
    ext4_put_le32(p + 4, (uint32_t)(value >> 32));
}

int ext4_range_ok(uint64_t raw_len, uint64_t offset, uint64_t size)
{
    if (size > UINT64_MAX - offset)
        return -RELIEFOS_EOVERFLOW;
    if (offset + size > raw_len)
        return -RELIEFOS_EINVAL;
    return 0;
}

int storage_ext4_parse_super(const uint8_t *raw, uint32_t raw_len,
                             struct storage_ext4_super_view *out)
{
    uint32_t log_block, log_cluster, block_size, cluster_size;
    uint32_t incompat, ro_compat, bpg, ipg, inode_size, desc_size;
    uint32_t i;
    uint64_t blocks_count, reserved_blocks, free_blocks, first_data_block;
    uint64_t data_blocks, group_count;

    if (raw == NULL || out == NULL)
        return -RELIEFOS_EINVAL;
    if (ext4_range_ok(raw_len, 0, EXT4_SUPERBLOCK_SIZE) != 0)
        return -RELIEFOS_EINVAL;
    if (ext4_get_le16(raw + SB_MAGIC) != EXT4_SUPER_MAGIC)
        return -RELIEFOS_EINVAL;

    /* Block size is 1024 << s_log_block_size; only 1/2/4 KiB exist. */
    log_block = ext4_get_le32(raw + SB_LOG_BLOCK_SIZE);
    if (log_block > 2)
        return -RELIEFOS_EINVAL;
    block_size = EXT4_MIN_BLOCK_SIZE << log_block;

    /* Cluster size is 1024 << s_log_cluster_size (ext4_handle_clustersize). */
    log_cluster = ext4_get_le32(raw + SB_LOG_CLUSTER_SIZE);
    if (log_cluster > MAX_LOG_CLUSTER_SIZE)
        return -RELIEFOS_EINVAL;
    cluster_size = EXT4_MIN_BLOCK_SIZE << log_cluster;

    incompat = ext4_get_le32(raw + SB_FEATURE_INCOMPAT);
    ro_compat = ext4_get_le32(raw + SB_FEATURE_RO_COMPAT);
    if ((ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) != 0) {
        if (cluster_size < block_size)
            return -RELIEFOS_EINVAL;
    } else if (cluster_size != block_size) {
        return -RELIEFOS_EINVAL;
    }

    bpg = ext4_get_le32(raw + SB_BLOCKS_PER_GROUP);
    ipg = ext4_get_le32(raw + SB_INODES_PER_GROUP);
    if (bpg == 0 || ipg == 0)
        return -RELIEFOS_EINVAL;
    if ((ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) == 0 &&
        bpg > block_size * 8)
        return -RELIEFOS_EINVAL;
    if (ipg > block_size * 8)
        return -RELIEFOS_EINVAL;

    /* Good-old rev implies 128-byte inodes regardless of the field. */
    if (ext4_get_le32(raw + SB_REV_LEVEL) == 0) {
        inode_size = EXT4_GOOD_OLD_INODE_SIZE;
    } else {
        inode_size = ext4_get_le16(raw + SB_INODE_SIZE);
        if (inode_size < EXT4_GOOD_OLD_INODE_SIZE ||
            (inode_size % EXT4_GOOD_OLD_INODE_SIZE) != 0 ||
            inode_size > block_size)
            return -RELIEFOS_EINVAL;
    }

    if ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0) {
        desc_size = ext4_get_le16(raw + SB_DESC_SIZE);
        if (desc_size < EXT4_MIN_DESC_SIZE || desc_size > EXT4_MAX_DESC_SIZE ||
            (desc_size & 3u) != 0)
            return -RELIEFOS_EINVAL;
    } else {
        desc_size = EXT4_MIN_DESC_SIZE;
    }

    /* The 32-bit high words only extend the counts with INCOMPAT_64BIT. */
    blocks_count = ext4_get_le32(raw + SB_BLOCKS_COUNT_LO);
    reserved_blocks = ext4_get_le32(raw + SB_R_BLOCKS_COUNT_LO);
    free_blocks = ext4_get_le32(raw + SB_FREE_BLOCKS_COUNT_LO);
    if ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0) {
        blocks_count |= (uint64_t)ext4_get_le32(raw + SB_BLOCKS_COUNT_HI) << 32;
        reserved_blocks |=
            (uint64_t)ext4_get_le32(raw + SB_R_BLOCKS_COUNT_HI) << 32;
        free_blocks |=
            (uint64_t)ext4_get_le32(raw + SB_FREE_BLOCKS_COUNT_HI) << 32;
    }

    first_data_block = ext4_get_le32(raw + SB_FIRST_DATA_BLOCK);
    if (blocks_count <= first_data_block)
        return -RELIEFOS_EINVAL;
    data_blocks = blocks_count - first_data_block;
    group_count = data_blocks / bpg + ((data_blocks % bpg) != 0 ? 1u : 0u);

    out->blocks_count = blocks_count;
    out->reserved_blocks_count = reserved_blocks;
    out->free_blocks_count = free_blocks;
    out->group_count = group_count;
    out->inodes_count = ext4_get_le32(raw + SB_INODES_COUNT);
    out->free_inodes_count = ext4_get_le32(raw + SB_FREE_INODES_COUNT);
    out->block_size = block_size;
    out->cluster_size = cluster_size;
    out->blocks_per_group = bpg;
    out->inodes_per_group = ipg;
    out->first_data_block = (uint32_t)first_data_block;
    out->desc_size = desc_size;
    out->inode_size = inode_size;
    out->feature_compat = ext4_get_le32(raw + SB_FEATURE_COMPAT);
    out->feature_incompat = incompat;
    out->feature_ro_compat = ro_compat;
    out->journal_inum = ext4_get_le32(raw + SB_JOURNAL_INUM);
    out->checksum_seed = ext4_get_le32(raw + SB_CHECKSUM_SEED);
    for (i = 0; i < 16; i++)
        out->uuid[i] = raw[SB_UUID + i];
    for (i = 0; i < 16; i++)
        out->volume_name[i] = raw[SB_VOLUME_NAME + i];
    out->volume_name[16] = '\0';
    return 0;
}

int storage_ext4_parse_group_desc(const uint8_t *raw, uint32_t raw_len,
                                  uint32_t desc_size, struct storage_ext4_group_view *out)
{
    uint64_t block_bitmap, inode_bitmap, inode_table;
    uint32_t free_blocks, free_inodes, used_dirs, itable_unused;
    int rc;

    if (raw == NULL || out == NULL)
        return -RELIEFOS_EINVAL;
    if (desc_size < EXT4_MIN_DESC_SIZE || desc_size > EXT4_MAX_DESC_SIZE ||
        (desc_size & 3u) != 0)
        return -RELIEFOS_EINVAL;
    rc = ext4_range_ok(raw_len, 0, desc_size);
    if (rc != 0)
        return rc;

    block_bitmap = ext4_get_le32(raw + GD_BLOCK_BITMAP_LO);
    inode_bitmap = ext4_get_le32(raw + GD_INODE_BITMAP_LO);
    inode_table = ext4_get_le32(raw + GD_INODE_TABLE_LO);
    free_blocks = ext4_get_le16(raw + GD_FREE_BLOCKS_LO);
    free_inodes = ext4_get_le16(raw + GD_FREE_INODES_LO);
    used_dirs = ext4_get_le16(raw + GD_USED_DIRS_LO);
    itable_unused = ext4_get_le16(raw + GD_ITABLE_UNUSED_LO);
    if (desc_size >= GD_HI_MIN_DESC_SIZE) {
        block_bitmap |= (uint64_t)ext4_get_le32(raw + GD_BLOCK_BITMAP_HI) << 32;
        inode_bitmap |= (uint64_t)ext4_get_le32(raw + GD_INODE_BITMAP_HI) << 32;
        inode_table |= (uint64_t)ext4_get_le32(raw + GD_INODE_TABLE_HI) << 32;
        free_blocks |= (uint32_t)ext4_get_le16(raw + GD_FREE_BLOCKS_HI) << 16;
        free_inodes |= (uint32_t)ext4_get_le16(raw + GD_FREE_INODES_HI) << 16;
        used_dirs |= (uint32_t)ext4_get_le16(raw + GD_USED_DIRS_HI) << 16;
        itable_unused |= (uint32_t)ext4_get_le16(raw + GD_ITABLE_UNUSED_HI) << 16;
    }

    out->block_bitmap = block_bitmap;
    out->inode_bitmap = inode_bitmap;
    out->inode_table = inode_table;
    out->free_blocks_count = free_blocks;
    out->free_inodes_count = free_inodes;
    out->used_dirs_count = used_dirs;
    out->itable_unused = itable_unused;
    out->flags = ext4_get_le16(raw + GD_FLAGS);
    out->checksum = ext4_get_le16(raw + GD_CHECKSUM);
    return 0;
}

int storage_ext4_parse_inode(const uint8_t *raw, uint32_t raw_len,
                             const struct storage_ext4_super_view *sb,
                             struct ext4_inode_view *out)
{
    if (!raw || !out || !sb || raw_len < 128 || raw_len > EXT4_MAX_BLOCK_SIZE)
        return -RELIEFOS_EINVAL;
    *out = (struct ext4_inode_view){0};
    out->mode = ext4_get_le16(raw);
    out->uid = ext4_get_le16(raw + 2) | ((uint32_t)ext4_get_le16(raw + 120) << 16);
    out->gid = ext4_get_le16(raw + 24) | ((uint32_t)ext4_get_le16(raw + 122) << 16);
    out->size = ext4_get_le32(raw + 4);
    if ((out->mode & EXT2_S_IFMT) == EXT2_S_IFREG ||
        (sb->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS))
        out->size |= (uint64_t)ext4_get_le32(raw + 108) << 32;
    out->atime = ext4_get_le32(raw + 8);
    out->ctime = ext4_get_le32(raw + 12);
    out->mtime = ext4_get_le32(raw + 16);
    out->dtime = ext4_get_le32(raw + 20);
    out->links_count = ext4_get_le16(raw + 26);
    out->blocks = ext4_get_le32(raw + 28);
    out->flags = ext4_get_le32(raw + 32);
    out->generation = ext4_get_le32(raw + 100);
    out->file_acl = ext4_get_le32(raw + 104);
    if (sb->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        out->file_acl |= (uint64_t)ext4_get_le16(raw + 118) << 32;
    if (sb->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE) {
        out->blocks |= (uint64_t)ext4_get_le16(raw + 116) << 32;
        if (out->flags & EXT4_HUGE_FILE_FL) {
            if (sb->block_size != 1024 && sb->block_size != 2048 && sb->block_size != 4096)
                return -RELIEFOS_EINVAL;
            out->blocks *= sb->block_size / SECTOR_SIZE;
        }
    }
    if (raw_len > 128) {
        if (raw_len < 132) return -RELIEFOS_EINVAL;
        out->extra_isize = ext4_get_le16(raw + 128);
        if (out->extra_isize > raw_len - 128 || (out->extra_isize & 3))
            return -RELIEFOS_EINVAL;
    }
    for (uint32_t i = 0; i < 60; ++i) out->i_block_raw[i] = raw[40 + i];
    return 0;
}

int storage_ext4_parse_extent_header(const uint8_t *raw, uint32_t raw_len,
                                     struct storage_ext4_extent_header_view *out)
{
    uint16_t magic, entries, max_entries, depth;
    int rc;

    if (raw == NULL || out == NULL)
        return -RELIEFOS_EINVAL;
    rc = ext4_range_ok(raw_len, 0, EH_SIZE);
    if (rc != 0)
        return rc;

    magic = ext4_get_le16(raw + EH_MAGIC);
    if (magic != EXT4_EXT_MAGIC)
        return -RELIEFOS_EINVAL;
    entries = ext4_get_le16(raw + EH_ENTRIES);
    max_entries = ext4_get_le16(raw + EH_MAX);
    depth = ext4_get_le16(raw + EH_DEPTH);
    if (entries > max_entries)
        return -RELIEFOS_EINVAL;
    if (depth > EXT4_MAX_EXTENT_DEPTH)
        return -RELIEFOS_EINVAL;

    out->magic = magic;
    out->entries = entries;
    out->max_entries = max_entries;
    out->depth = depth;
    out->generation = ext4_get_le32(raw + EH_GENERATION);
    return 0;
}

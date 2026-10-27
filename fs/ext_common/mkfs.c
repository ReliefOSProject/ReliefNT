/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/export.h>
#include "internal.h"

/**
 * @brief Writes one 4 KiB ext-family block during installer target formatting.
 * @param disk Borrowed managed target receiving the block under execution ownership.
 * @param start_lba First LBA of the ext-family partition.
 * @param block Zero-based filesystem block number.
 * @param data Complete 4 KiB block contents.
 * @return Zero on success or a negative storage error.
 */
static int install_write_ext2_block(struct install_disk_state *disk, uint64_t start_lba,
                                    uint32_t block, const void *data)
{
    return install_write_sectors(disk, start_lba + (uint64_t)block * 8u, 8u, data);
}

/**
 * @brief Marks a bit in an in-memory ext-family bitmap block.
 * @param bitmap Writable 4 KiB bitmap storage.
 * @param bit Zero-based block or inode bit to mark allocated.
 */
static void install_ext2_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8u] |= (uint8_t)(1u << (bit & 7u));
}

/**
 * @brief Builds one ext-family block-group descriptor.
 * @param descriptor Writable descriptor receiving the group's metadata locations.
 * @param group Zero-based block-group index.
 * @param group_blocks Blocks available in the group.
 * @param descriptor_blocks Number of 4 KiB blocks occupied by the descriptor table.
 * @param inode_table_blocks Number of blocks occupied by one inode table.
 * @return Zero when the group has enough space for its metadata, otherwise -ENOSPC.
 */
static int install_ext2_make_group_desc(struct ext2_group_desc *descriptor,
                                        uint32_t group, uint32_t group_blocks,
                                        uint32_t descriptor_blocks,
                                        uint32_t inode_table_blocks)
{
    uint32_t group_start;
    uint32_t metadata_blocks;
    uint32_t allocated_blocks;
    uint32_t allocated_inodes;

    if (!descriptor || descriptor_blocks == 0u) {
        return -22;
    }
    group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
    /* Every group stores a superblock plus a complete backup descriptor table,
     * then its block bitmap, inode bitmap, and inode table. */
    metadata_blocks = 3u + descriptor_blocks + inode_table_blocks;
    allocated_blocks = metadata_blocks + (group == 0u ? 1u : 0u);
    allocated_inodes = group == 0u ? 10u : 0u;
    if (group_blocks <= allocated_blocks) {
        return -28;
    }

    storage_memzero(descriptor, sizeof(*descriptor));
    descriptor->block_bitmap = group_start + 1u + descriptor_blocks;
    descriptor->inode_bitmap = descriptor->block_bitmap + 1u;
    descriptor->inode_table = descriptor->inode_bitmap + 1u;
    descriptor->free_blocks_count = (uint16_t)(group_blocks - allocated_blocks);
    descriptor->free_inodes_count =
        (uint16_t)(INSTALL_EXT2_INODES_PER_GROUP - allocated_inodes);
    descriptor->used_dirs_count = group == 0u ? 1u : 0u;
    return 0;
}

/**
 * @brief Creates a writable ext2-compatible filesystem for the installed root.
 *
 * The ext4 variant adds the extents incompatibility feature and stores the
 * root directory through a valid depth-zero extent tree. The shared layout
 * keeps the formatter small while allowing Linux ext4 tooling to inspect and
 * mount the installed target.
 * @param disk Block disk containing the target partition.
 * @param start_lba First sector of the ext-family partition.
 * @param sector_count Number of sectors available to the filesystem.
 * @param ext4 True for extent-based ext4, false for classic indirect ext2.
 * @return Zero or layout/I/O errno. Execution ownership and the admitted
 * provider pin span all writes; no graph/module lock or deferred work.
 */
static int install_format_ext_family(struct install_disk_state *disk, uint64_t start_lba,
                                     uint64_t sector_count, bool ext4)
{
    uint32_t blocks;
    uint32_t group_count;
    uint32_t descriptors_per_block;
    uint32_t descriptor_blocks;
    uint32_t inode_table_blocks;
    uint32_t inodes_count;
    uint32_t free_blocks = 0;
    uint32_t free_inodes = 0;
    struct ext2_superblock super;
    int ret;
    if (!disk || sector_count < 262144ULL || sector_count / 8u > 0xffffffffULL) return -28;
    blocks = (uint32_t)(sector_count / 8u);
    group_count = (blocks + INSTALL_EXT2_BLOCKS_PER_GROUP - 1u) /
                  INSTALL_EXT2_BLOCKS_PER_GROUP;
    descriptors_per_block = INSTALL_EXT2_BLOCK_SIZE / sizeof(struct ext2_group_desc);
    descriptor_blocks = (group_count + descriptors_per_block - 1u) / descriptors_per_block;
    inode_table_blocks = (INSTALL_EXT2_INODES_PER_GROUP * 128u) / INSTALL_EXT2_BLOCK_SIZE;
    inodes_count = group_count * INSTALL_EXT2_INODES_PER_GROUP;
    if (!group_count || !descriptor_blocks ||
        blocks < (uint64_t)inode_table_blocks + descriptor_blocks + 4u) return -28;

    storage_memzero(&super, sizeof(super));
    for (uint32_t group = 0; group < group_count; ++group) {
        uint32_t group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
        uint32_t group_blocks = min_u32(INSTALL_EXT2_BLOCKS_PER_GROUP, blocks - group_start);
        struct ext2_group_desc descriptor;
        ret = install_ext2_make_group_desc(&descriptor, group, group_blocks,
                                           descriptor_blocks, inode_table_blocks);
        if (ret < 0) return ret;
        free_blocks += descriptor.free_blocks_count;
        free_inodes += descriptor.free_inodes_count;
    }

    super.inodes_count = inodes_count;
    super.blocks_count = blocks;
    super.free_blocks_count = free_blocks;
    super.free_inodes_count = free_inodes;
    super.first_data_block = 0;
    super.log_block_size = 2u;
    super.log_frag_size = 2;
    super.blocks_per_group = INSTALL_EXT2_BLOCKS_PER_GROUP;
    super.frags_per_group = INSTALL_EXT2_BLOCKS_PER_GROUP;
    super.inodes_per_group = INSTALL_EXT2_INODES_PER_GROUP;
    super.magic = EXT2_SUPER_MAGIC;
    super.state = 1u;
    super.errors = 1u;
    super.rev_level = EXT2_DYNAMIC_REV;
    super.first_ino = 11u;
    super.inode_size = 128u;
    super.feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                             (ext4 ? EXT4_FEATURE_INCOMPAT_EXTENTS : 0u);
    storage_memcpy(super.volume_name, "RELIEFOS", 8u);

    /* A non-sparse classic layout keeps a backup superblock and a complete
     * descriptor table at the beginning of every group. Generate one table
     * block at a time: large filesystems need more than the single 4 KiB
     * descriptor block used by the original formatter. */
    for (uint32_t group = 0; group < group_count; ++group) {
        uint32_t group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        if (group == 0) {
            storage_memcpy(storage_scratch + EXT2_SUPERBLOCK_OFFSET, &super, sizeof(super));
        } else {
            storage_memcpy(storage_scratch, &super, sizeof(super));
        }
        ret = install_write_ext2_block(disk, start_lba, group_start, storage_scratch);
        if (ret < 0) return ret;
        for (uint32_t table_block = 0; table_block < descriptor_blocks; ++table_block) {
            struct ext2_group_desc *descriptors =
                (struct ext2_group_desc *)(void *)storage_scratch;
            uint32_t first_descriptor = table_block * descriptors_per_block;
            storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
            for (uint32_t slot = 0; slot < descriptors_per_block; ++slot) {
                uint32_t descriptor_group = first_descriptor + slot;
                uint32_t descriptor_start;
                uint32_t descriptor_group_blocks;
                if (descriptor_group >= group_count) {
                    break;
                }
                descriptor_start = descriptor_group * INSTALL_EXT2_BLOCKS_PER_GROUP;
                descriptor_group_blocks = min_u32(INSTALL_EXT2_BLOCKS_PER_GROUP,
                                                   blocks - descriptor_start);
                ret = install_ext2_make_group_desc(&descriptors[slot], descriptor_group,
                                                   descriptor_group_blocks,
                                                   descriptor_blocks, inode_table_blocks);
                if (ret < 0) return ret;
            }
            ret = install_write_ext2_block(disk, start_lba,
                                           group_start + 1u + table_block, storage_scratch);
            if (ret < 0) return ret;
        }
    }

    for (uint32_t group = 0; group < group_count; ++group) {
        uint32_t group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
        uint32_t group_blocks = min_u32(INSTALL_EXT2_BLOCKS_PER_GROUP,
                                        blocks - group_start);
        uint32_t metadata_blocks = 3u + descriptor_blocks + inode_table_blocks;
        uint32_t allocated_blocks = metadata_blocks + (group == 0u ? 1u : 0u);
        uint32_t block_bitmap = group_start + 1u + descriptor_blocks;
        uint32_t inode_bitmap = block_bitmap + 1u;
        uint32_t inode_table = inode_bitmap + 1u;
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        for (uint32_t bit = 0; bit < allocated_blocks; ++bit) install_ext2_set_bit(storage_scratch, bit);
        /* e2fsck requires unavailable tail bits in the final block group to
         * be allocated.  They are outside the free-block counters. */
        for (uint32_t bit = group_blocks;
             bit < INSTALL_EXT2_BLOCK_SIZE * 8u; ++bit) {
            install_ext2_set_bit(storage_scratch, bit);
        }
        ret = install_write_ext2_block(disk, start_lba, block_bitmap, storage_scratch);
        if (ret < 0) return ret;
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        if (group == 0) {
            for (uint32_t bit = 0; bit < 10u; ++bit) install_ext2_set_bit(storage_scratch, bit);
        }
        /* Each bitmap block contains more bits than this filesystem exposes
         * as inodes.  Mark the padding bits allocated in every group. */
        for (uint32_t bit = INSTALL_EXT2_INODES_PER_GROUP;
             bit < INSTALL_EXT2_BLOCK_SIZE * 8u; ++bit) {
            install_ext2_set_bit(storage_scratch, bit);
        }
        ret = install_write_ext2_block(disk, start_lba, inode_bitmap, storage_scratch);
        if (ret < 0) return ret;
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        for (uint32_t table = 0; table < inode_table_blocks; ++table) {
            ret = install_write_ext2_block(disk, start_lba, inode_table + table, storage_scratch);
            if (ret < 0) return ret;
        }
    }

    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    {
        struct ext2_inode *root = (struct ext2_inode *)(void *)(storage_scratch + 128u);
        uint32_t root_block = 3u + descriptor_blocks + inode_table_blocks;
        root->mode = EXT2_S_IFDIR | 0755u;
        root->size_lo = INSTALL_EXT2_BLOCK_SIZE;
        root->links_count = 2u;
        root->blocks_512 = 8u;
        if (ext4) {
            /* A depth-zero extent tree fits in i_block. */
            root->flags = EXT4_EXTENTS_FL;
            root->block[0] = 0x0001f30au; /* magic 0xf30a, entries 1 */
            root->block[1] = 4u;          /* max entries 4, depth 0 */
            root->block[2] = 0u;          /* extent generation */
            root->block[3] = 0u;          /* logical block 0 */
            root->block[4] = 1u;          /* length 1, start_hi 0 */
            root->block[5] = root_block;  /* physical block */
        } else {
            root->block[0] = root_block;
        }
    }
    ret = install_write_ext2_block(disk, start_lba, 3u + descriptor_blocks, storage_scratch);
    if (ret < 0) return ret;
    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    {
        struct ext2_dirent *dot = (struct ext2_dirent *)(void *)storage_scratch;
        struct ext2_dirent *dotdot = (struct ext2_dirent *)(void *)(storage_scratch + 12u);
        dot->inode = EXT2_ROOT_INO;
        dot->rec_len = 12u;
        dot->name_len = 1u;
        dot->file_type = EXT2_FT_DIR;
        dot->name[0] = '.';
        dotdot->inode = EXT2_ROOT_INO;
        dotdot->rec_len = INSTALL_EXT2_BLOCK_SIZE - 12u;
        dotdot->name_len = 2u;
        dotdot->file_type = EXT2_FT_DIR;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
    }
    return install_write_ext2_block(disk, start_lba,
                                    3u + descriptor_blocks + inode_table_blocks,
                                    storage_scratch);
}

/** @brief Write native ext2 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target disk. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted filesystem
 * provider pin required across all writes; no graph/module lock or queued work.
 */
int install_format_ext2(struct install_disk_state *disk, uint64_t start_lba,
                               uint64_t sector_count)
{
    return install_format_ext_family(disk, start_lba, sector_count, false);
}

/** @brief Write native ext4 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target disk. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted filesystem
 * provider pin required across all writes; no graph/module lock or queued work.
 */
int install_format_ext4(struct install_disk_state *disk, uint64_t start_lba,
                               uint64_t sector_count)
{
    return install_format_ext_family(disk, start_lba, sector_count, true);
}


EXPORT_SYMBOL(install_format_ext2);
EXPORT_SYMBOL(install_format_ext4);

/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/export.h>
#include "internal.h"

/* ext3 uses the same group layout as ext2, with a reserved inode 8 journal.
 * The journal's maximum default (1 GiB) fits the double-indirect map. */
#define INSTALL_EXT3_POINTERS_PER_BLOCK (INSTALL_EXT2_BLOCK_SIZE / 4u)
#define INSTALL_EXT3_DIRECT_BLOCKS 12u

/** @brief Store a little-endian on-disk word without alignment assumptions.
 * @param bytes Writable four-byte destination. @param value Host-order word.
 */
static void install_ext3_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/** @brief Store a JBD big-endian on-disk word without alignment assumptions.
 * @param bytes Writable four-byte destination. @param value Host-order word.
 */
static void install_ext3_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

/** @brief Choose the standard e2fsprogs default journal size for 4 KiB blocks.
 * @param blocks Filesystem block count, already validated against the minimum.
 * @return Number of journal data blocks, excluding indirect blocks.
 */
static uint32_t install_ext3_journal_size(uint32_t blocks)
{
    static const struct { uint32_t limit, journal; } choices[] = {
        {32768u, 1024u}, {262144u, 4096u}, {524288u, 8192u},
        {4194304u, 16384u}, {8388608u, 32768u},
        {16777216u, 65536u}, {33554432u, 131072u},
    };
    for (uint32_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i)
        if (blocks < choices[i].limit) return choices[i].journal;
    return 262144u;
}

/** @brief Count journal indirect blocks for a classic ext inode block map.
 * @param data_blocks Journal file length in 4 KiB data blocks.
 * @return Number of single and double indirect mapping blocks.
 */
static uint32_t install_ext3_map_blocks(uint32_t data_blocks)
{
    if (data_blocks <= INSTALL_EXT3_DIRECT_BLOCKS) return 0;
    if (data_blocks <= INSTALL_EXT3_DIRECT_BLOCKS + INSTALL_EXT3_POINTERS_PER_BLOCK) return 1u;
    uint32_t double_data = data_blocks - INSTALL_EXT3_DIRECT_BLOCKS - INSTALL_EXT3_POINTERS_PER_BLOCK;
    return 2u + (double_data + INSTALL_EXT3_POINTERS_PER_BLOCK - 1u) /
                INSTALL_EXT3_POINTERS_PER_BLOCK;
}

/** @brief Count consecutive journal allocations in one block group.
 * @param group Zero-based group. @param group_blocks Available blocks in group.
 * @param metadata_blocks Fixed superblock, descriptor, bitmap and inode-table blocks.
 * @param journal_blocks Total journal data and indirect blocks.
 * @return Allocated journal blocks in this group, excluding root and metadata.
 */
static uint32_t install_ext3_group_blocks(uint32_t group, uint32_t group_blocks,
                                          uint32_t metadata_blocks, uint32_t journal_blocks)
{
    uint32_t first_capacity = INSTALL_EXT2_BLOCKS_PER_GROUP - metadata_blocks - 1u;
    uint32_t capacity = INSTALL_EXT2_BLOCKS_PER_GROUP - metadata_blocks;
    uint64_t first = group ? (uint64_t)first_capacity + (uint64_t)(group - 1u) * capacity : 0u;
    uint32_t available = group_blocks - metadata_blocks - (group == 0u ? 1u : 0u);
    if (first >= journal_blocks) return 0;
    return min_u32(available, journal_blocks - (uint32_t)first);
}

/** @brief Map a journal allocation ordinal through groups while skipping metadata.
 * @param ordinal Zero-based journal data or indirect allocation ordinal.
 * @param metadata_blocks Fixed blocks at the start of each group.
 * @return Filesystem block holding the allocated ordinal.
 */
static uint32_t install_ext3_journal_block(uint32_t ordinal, uint32_t metadata_blocks)
{
    uint32_t first_capacity = INSTALL_EXT2_BLOCKS_PER_GROUP - metadata_blocks - 1u;
    if (ordinal < first_capacity) return metadata_blocks + 1u + ordinal;
    ordinal -= first_capacity;
    uint32_t capacity = INSTALL_EXT2_BLOCKS_PER_GROUP - metadata_blocks;
    uint32_t group = 1u + ordinal / capacity;
    return group * INSTALL_EXT2_BLOCKS_PER_GROUP + metadata_blocks + ordinal % capacity;
}

/** @brief Flush a pinned formatter target after ordering its sector writes.
 * @param volume Borrowed formatter volume with a live block-provider reference.
 * @return Zero or the provider's negative flush error.
 */
static int install_ext3_flush(const struct storage_volume *volume)
{
    bool async = storage_io_async_context;
    storage_io_async_context = false;
    int ret = storage_ext4_device_flush(volume);
    storage_io_async_context = async;
    return ret;
}

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
 * @param journal_blocks Journal data and map blocks allocated in this group.
 * @return Zero when the group has enough space for its metadata, otherwise -ENOSPC.
 */
static int install_ext2_make_group_desc(struct ext2_group_desc *descriptor,
                                        uint32_t group, uint32_t group_blocks,
                                        uint32_t descriptor_blocks,
                                        uint32_t inode_table_blocks,
                                        uint32_t journal_blocks)
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
    allocated_blocks = metadata_blocks + (group == 0u ? 1u : 0u) + journal_blocks;
    allocated_inodes = group == 0u ? 10u : 0u;
    if (group_blocks < allocated_blocks) {
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

/** @brief Construct journal inode 8 with classic direct and indirect pointers.
 * @param inode Writable journal inode. @param data_blocks Journal data length.
 * @param metadata_blocks Fixed group metadata length in blocks.
 */
static void install_ext3_make_inode(struct ext2_inode *inode, uint32_t data_blocks,
                                    uint32_t metadata_blocks)
{
    storage_memzero(inode, sizeof(*inode));
    inode->mode = EXT2_S_IFREG | 0600u;
    inode->size_lo = data_blocks * INSTALL_EXT2_BLOCK_SIZE;
    inode->links_count = 1u;
    inode->blocks_512 = (data_blocks + install_ext3_map_blocks(data_blocks)) * 8u;
    for (uint32_t i = 0; i < INSTALL_EXT3_DIRECT_BLOCKS && i < data_blocks; ++i)
        inode->block[i] = install_ext3_journal_block(i, metadata_blocks);
    if (data_blocks > INSTALL_EXT3_DIRECT_BLOCKS)
        inode->block[12] = install_ext3_journal_block(data_blocks, metadata_blocks);
    if (data_blocks > INSTALL_EXT3_DIRECT_BLOCKS + INSTALL_EXT3_POINTERS_PER_BLOCK)
        inode->block[13] = install_ext3_journal_block(data_blocks + 1u, metadata_blocks);
}

/** @brief Serialize one complete ext3 superblock and its journal inode backup.
 * @param raw Writable zeroed superblock bytes. @param super Common layout fields.
 * @param inode Journal inode whose block map and size are duplicated.
 * @param group Group containing this superblock copy.
 */
static void install_ext3_super_bytes(uint8_t *raw, const struct ext2_superblock *super,
                                     const struct ext2_inode *inode, uint32_t group)
{
    storage_memcpy(raw, super, sizeof(*super));
    raw[0x5a] = (uint8_t)group;
    raw[0x5b] = (uint8_t)(group >> 8);
    install_ext3_le32(raw + 0xe0, 8u);
    raw[0xfd] = 1u; /* The backup contains the journal inode's block pointers. */
    for (uint32_t i = 0; i < 15u; ++i)
        install_ext3_le32(raw + 0x10c + i * 4u, inode->block[i]);
    install_ext3_le32(raw + 0x148, inode->size_high);
    install_ext3_le32(raw + 0x14c, inode->size_lo);
}

/** @brief Initialize journal data, JBD superblock and indirect mapping blocks.
 * @param disk Borrowed writable target. @param start_lba First filesystem sector.
 * @param data_blocks Journal length in data blocks. @param metadata_blocks Group prefix.
 * @param uuid Filesystem UUID used by the journal. @return Zero or first I/O error.
 */
static int install_ext3_write_journal(struct install_disk_state *disk, uint64_t start_lba,
                                      uint32_t data_blocks, uint32_t metadata_blocks,
                                      const uint8_t *uuid)
{
    int ret;
    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    for (uint32_t i = 0; i < data_blocks; ++i) {
        ret = install_write_ext2_block(disk, start_lba,
                                       install_ext3_journal_block(i, metadata_blocks),
                                       storage_scratch);
        if (ret < 0) return ret;
    }
    /* JBD v2 fields are big-endian, unlike the ext filesystem structures. */
    install_ext3_be32(storage_scratch + 0x00, 0xc03b3998u);
    install_ext3_be32(storage_scratch + 0x04, 4u);
    install_ext3_be32(storage_scratch + 0x0c, INSTALL_EXT2_BLOCK_SIZE);
    install_ext3_be32(storage_scratch + 0x10, data_blocks);
    install_ext3_be32(storage_scratch + 0x14, 1u);
    install_ext3_be32(storage_scratch + 0x18, 1u);
    storage_memcpy(storage_scratch + 0x30, uuid, 16u);
    install_ext3_be32(storage_scratch + 0x40, 1u);
    ret = install_write_ext2_block(disk, start_lba,
                                   install_ext3_journal_block(0, metadata_blocks), storage_scratch);
    if (ret < 0) return ret;

    if (data_blocks <= INSTALL_EXT3_DIRECT_BLOCKS) return 0;
    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    uint32_t single = min_u32(data_blocks - INSTALL_EXT3_DIRECT_BLOCKS,
                              INSTALL_EXT3_POINTERS_PER_BLOCK);
    for (uint32_t i = 0; i < single; ++i)
        install_ext3_le32(storage_scratch + i * 4u,
                          install_ext3_journal_block(INSTALL_EXT3_DIRECT_BLOCKS + i, metadata_blocks));
    ret = install_write_ext2_block(disk, start_lba,
                                   install_ext3_journal_block(data_blocks, metadata_blocks), storage_scratch);
    if (ret < 0) return ret;

    uint32_t double_first = INSTALL_EXT3_DIRECT_BLOCKS + INSTALL_EXT3_POINTERS_PER_BLOCK;
    if (data_blocks <= double_first) return 0;
    uint32_t remaining = data_blocks - double_first;
    uint32_t leaves = (remaining + INSTALL_EXT3_POINTERS_PER_BLOCK - 1u) /
                      INSTALL_EXT3_POINTERS_PER_BLOCK;
    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    for (uint32_t i = 0; i < leaves; ++i)
        install_ext3_le32(storage_scratch + i * 4u,
                          install_ext3_journal_block(data_blocks + 2u + i, metadata_blocks));
    ret = install_write_ext2_block(disk, start_lba,
                                   install_ext3_journal_block(data_blocks + 1u, metadata_blocks), storage_scratch);
    if (ret < 0) return ret;
    for (uint32_t leaf = 0; leaf < leaves; ++leaf) {
        uint32_t first = double_first + leaf * INSTALL_EXT3_POINTERS_PER_BLOCK;
        uint32_t count = min_u32(data_blocks - first, INSTALL_EXT3_POINTERS_PER_BLOCK);
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        for (uint32_t i = 0; i < count; ++i)
            install_ext3_le32(storage_scratch + i * 4u,
                              install_ext3_journal_block(first + i, metadata_blocks));
        ret = install_write_ext2_block(disk, start_lba,
                                       install_ext3_journal_block(data_blocks + 2u + leaf, metadata_blocks),
                                       storage_scratch);
        if (ret < 0) return ret;
    }
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
 * @param ext4 True for extent-based ext4, false for classic block mapping.
 * @param ext3_volume Non-NULL pinned target selects an internal ext3 journal.
 * @return Zero or layout/I/O errno. Execution ownership and the admitted
 * provider pin span all writes; no graph/module lock or deferred work.
 */
static int install_format_ext_family(struct install_disk_state *disk, uint64_t start_lba,
                                     uint64_t sector_count, bool ext4,
                                     const struct storage_volume *ext3_volume)
{
    uint32_t blocks;
    uint32_t group_count;
    uint32_t descriptors_per_block;
    uint32_t descriptor_blocks;
    uint32_t inode_table_blocks;
    uint32_t inodes_count;
    uint32_t free_blocks = 0;
    uint32_t free_inodes = 0;
    uint32_t journal_data = 0, journal_blocks = 0, journal_allocated = 0, metadata_blocks;
    struct ext2_superblock super;
    struct ext2_inode journal_inode;
    int ret;
    if (!disk || sector_count < (ext3_volume ? 16384ULL : 262144ULL) ||
        sector_count / 8u > 0xffffffffULL) return -28;
    blocks = (uint32_t)(sector_count / 8u);
    group_count = (blocks - 1u) / INSTALL_EXT2_BLOCKS_PER_GROUP + 1u;
    descriptors_per_block = INSTALL_EXT2_BLOCK_SIZE / sizeof(struct ext2_group_desc);
    descriptor_blocks = (group_count + descriptors_per_block - 1u) / descriptors_per_block;
    inode_table_blocks = (INSTALL_EXT2_INODES_PER_GROUP * 128u) / INSTALL_EXT2_BLOCK_SIZE;
    /* Drop only a final group too small to hold its mandatory metadata. */
    if (ext3_volume && group_count > 1u &&
        blocks - (group_count - 1u) * INSTALL_EXT2_BLOCKS_PER_GROUP <=
            3u + descriptor_blocks + inode_table_blocks) {
        blocks = (group_count - 1u) * INSTALL_EXT2_BLOCKS_PER_GROUP;
        --group_count;
        descriptor_blocks = (group_count + descriptors_per_block - 1u) / descriptors_per_block;
    }
    inodes_count = group_count * INSTALL_EXT2_INODES_PER_GROUP;
    metadata_blocks = 3u + descriptor_blocks + inode_table_blocks;
    if (ext3_volume) {
        journal_data = install_ext3_journal_size(blocks);
        journal_blocks = journal_data + install_ext3_map_blocks(journal_data);
        install_ext3_make_inode(&journal_inode, journal_data, metadata_blocks);
    }
    if (!group_count || !descriptor_blocks ||
        blocks < (uint64_t)inode_table_blocks + descriptor_blocks + 4u) return -28;

    storage_memzero(&super, sizeof(super));
    for (uint32_t group = 0; group < group_count; ++group) {
        uint32_t group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
        uint32_t group_blocks = min_u32(INSTALL_EXT2_BLOCKS_PER_GROUP, blocks - group_start);
        struct ext2_group_desc descriptor;
        if (group_blocks <= metadata_blocks + (group == 0u ? 1u : 0u)) return -28;
        uint32_t journal_group = install_ext3_group_blocks(group, group_blocks,
                                                            metadata_blocks, journal_blocks);
        journal_allocated += journal_group;
        ret = install_ext2_make_group_desc(&descriptor, group, group_blocks,
                                           descriptor_blocks, inode_table_blocks, journal_group);
        if (ret < 0) return ret;
        free_blocks += descriptor.free_blocks_count;
        free_inodes += descriptor.free_inodes_count;
    }
    if (journal_allocated != journal_blocks) return -28;

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
    super.feature_compat = ext3_volume ? EXT4_FEATURE_COMPAT_HAS_JOURNAL : 0u;
    super.feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
                             (ext4 ? EXT4_FEATURE_INCOMPAT_EXTENTS : 0u);
    storage_memcpy(super.volume_name, "RELIEFOS", 8u);

    if (ext3_volume) {
        /* Invalidate old primary and backup copies before changing group data. */
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        ret = install_write_ext2_block(disk, start_lba, 0, storage_scratch);
        if (ret < 0) return ret;
        for (uint32_t group = 1; group < group_count; ++group) {
            ret = install_write_ext2_block(disk, start_lba,
                                           group * INSTALL_EXT2_BLOCKS_PER_GROUP, storage_scratch);
            if (ret < 0) return ret;
        }
        ret = install_ext3_flush(ext3_volume);
        if (ret < 0) return ret;
    }

    /* A non-sparse classic layout keeps a backup superblock and a complete
     * descriptor table at the beginning of every group. Generate one table
     * block at a time: large filesystems need more than the single 4 KiB
     * descriptor block used by the original formatter. */
    for (uint32_t group = 0; group < group_count; ++group) {
        uint32_t group_start = group * INSTALL_EXT2_BLOCKS_PER_GROUP;
        if (!ext3_volume) {
            storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
            if (group == 0) {
                storage_memcpy(storage_scratch + EXT2_SUPERBLOCK_OFFSET, &super, sizeof(super));
            } else {
                storage_memcpy(storage_scratch, &super, sizeof(super));
            }
            ret = install_write_ext2_block(disk, start_lba, group_start, storage_scratch);
            if (ret < 0) return ret;
        }
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
                uint32_t journal_group = install_ext3_group_blocks(descriptor_group,
                    descriptor_group_blocks, metadata_blocks, journal_blocks);
                ret = install_ext2_make_group_desc(&descriptors[slot], descriptor_group,
                                                   descriptor_group_blocks,
                                                   descriptor_blocks, inode_table_blocks,
                                                   journal_group);
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
        uint32_t allocated_blocks = metadata_blocks + (group == 0u ? 1u : 0u) +
            install_ext3_group_blocks(group, group_blocks, metadata_blocks, journal_blocks);
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
        if (ext3_volume) {
            struct ext2_inode *journal = (struct ext2_inode *)(void *)(storage_scratch + 7u * 128u);
            storage_memcpy(journal, &journal_inode, sizeof(journal_inode));
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
    ret = install_write_ext2_block(disk, start_lba,
                                    3u + descriptor_blocks + inode_table_blocks, storage_scratch);
    if (ret < 0 || !ext3_volume) return ret;

    ret = install_ext3_write_journal(disk, start_lba, journal_data, metadata_blocks, super.uuid);
    if (ret < 0) return ret;
    /* Journal and all group metadata must be durable before any valid copy is published. */
    ret = install_ext3_flush(ext3_volume);
    if (ret < 0) return ret;
    for (uint32_t group = 1; group < group_count; ++group) {
        storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
        install_ext3_super_bytes(storage_scratch, &super, &journal_inode, group);
        ret = install_write_ext2_block(disk, start_lba,
                                       group * INSTALL_EXT2_BLOCKS_PER_GROUP, storage_scratch);
        if (ret < 0) return ret;
    }
    ret = install_ext3_flush(ext3_volume);
    if (ret < 0) return ret;
    storage_memzero(storage_scratch, INSTALL_EXT2_BLOCK_SIZE);
    install_ext3_super_bytes(storage_scratch + EXT2_SUPERBLOCK_OFFSET, &super, &journal_inode, 0);
    ret = install_write_ext2_block(disk, start_lba, 0, storage_scratch);
    if (ret < 0) return ret;
    return install_ext3_flush(ext3_volume);
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
    return install_format_ext_family(disk, start_lba, sector_count, false, NULL);
}

/** @brief Format an authorized extent as ext3 with an internal JBD journal.
 * @param disk Borrowed present managed disk, stable under execution ownership.
 * @param start_lba First sector of the validated target extent.
 * @param sector_count Extent length in 512-byte sectors.
 * @return Zero after metadata and primary-superblock flush, or negative
 * validation, provider, layout, write or flush errno; partial writes may remain.
 */
int install_format_ext3(struct install_disk_state *disk, uint64_t start_lba,
                        uint64_t sector_count)
{
    if (!disk || !disk->present) return -22;
    /* e2fsprogs' smallest default journal is 1024 blocks on a 2048-block fs. */
    if (sector_count < 16384ULL || sector_count / 8u > 0xffffffffULL) return -28;
    if (start_lba > UINT64_MAX - sector_count) return -22;

    struct storage_volume volume = {0};
    int ret = block_device_get(disk->device, &volume.device_ref);
    if (ret < 0) return ret;
    struct block_device_info info;
    ret = block_device_info(&volume.device_ref, &info);
    if (!ret && info.block_size != 512u) ret = -95;
    if (!ret && (info.flags & BLOCK_DEVICE_READ_ONLY)) ret = -30;
    if (!ret && (start_lba >= info.blocks || sector_count > info.blocks - start_lba)) ret = -22;
    if (!ret) ret = install_format_ext_family(disk, start_lba, sector_count, false, &volume);
    block_device_put(&volume.device_ref);
    return ret;
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
    return install_format_ext_family(disk, start_lba, sector_count, true, NULL);
}


EXPORT_SYMBOL(install_format_ext2);
EXPORT_SYMBOL(install_format_ext3);
EXPORT_SYMBOL(install_format_ext4);

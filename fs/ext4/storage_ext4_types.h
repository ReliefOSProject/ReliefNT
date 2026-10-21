/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXT4_STORAGE_EXT4_TYPES_H
#define RELIEFNT_EXT4_STORAGE_EXT4_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file storage_ext4_types.h
 * @brief Private ext4 on-disk views and per-volume state layouts.
 */

/* ---- ext4 on-disk format --------------------------------------------
 * Native-integer views over the little-endian ext4 on-disk layout
 * (linux/fs/ext4/ext4.h, Linux v7.3-rc5 reference).  Packed disk structs
 * stay private to storage_ext4_format.c; callers only see these views.
 * All parsers return 0 or a negative RELIEFOS_* errno: format violations
 * (bad magic, illegal geometry, out-of-range reference) return
 * -RELIEFOS_EINVAL; offset+size arithmetic overflow returns
 * -RELIEFOS_EOVERFLOW. */

/* Linux asm-generic value; reliefnt/syscall.h has no alias yet. */
#define RELIEFOS_EOVERFLOW 75
#ifndef RELIEFOS_EFBIG
#define RELIEFOS_EFBIG 27
#endif
#define EXT4_EXTENTS_FL 0x00080000u
#define EXT4_HUGE_FILE_FL 0x00040000u
#define EXT4_MAX_LOGICAL_BLOCK 0xfffffffeu
#ifndef EXT4_READAHEAD_BLOCKS
#define EXT4_READAHEAD_BLOCKS 32u
#endif

#define EXT4_SUPERBLOCK_OFFSET 1024u
#define EXT4_SUPERBLOCK_SIZE 1024u
/* On-disk offset of s_checksum_seed (linux/fs/ext4/ext4.h:1369).  Named
 * here so storage_ext4_parse_super and the host tests share one spelling;
 * keep the replacement list token-identical to the tests' local copy. */
#define SB_CHECKSUM_SEED 0x270
/* On-disk offset of s_creator_os (ext4.h:1369).  EXT4_OS_LINUX is 0; inode
 * checksums are only meaningful for Linux-created filesystems, so the mount
 * path reads this word before calling storage_ext4_verify_inode_checksum. */
#define SB_CREATOR_OS 0x48
/* On-disk offsets of s_reserved_gdt_blocks / s_log_groups_per_flex /
 * s_backup_bgs (ext4.h:1369).  storage_ext4_mount() reads these raw words
 * into the allocator context because storage_ext4_parse_super() does not
 * expose them (same shared-spelling convention as SB_CHECKSUM_SEED).
 * SB_BACKUP_BGS is s_backup_bgs[2] at 0x24C (after s_mount_opts at 0x200
 * and the quota/overhead words); verified against a real sparse_super2
 * mke2fs image ("Backup block groups: 1 7" == the le32 pair at 0x24C). */
#define SB_RESERVED_GDT 0xCE
#define SB_LOG_GROUPS_PER_FLEX 0x174
#define SB_BACKUP_BGS 0x24C
#define EXT4_SUPER_MAGIC 0xef53u
#define EXT4_EXT_MAGIC 0xf30au
#define EXT4_MIN_BLOCK_SIZE 1024u
#define EXT4_MAX_BLOCK_SIZE 4096u
#define EXT4_GOOD_OLD_INODE_SIZE 128u
#define EXT4_MIN_DESC_SIZE 32u
#define EXT4_MAX_DESC_SIZE 256u
#define EXT4_MAX_EXTENT_DEPTH 5u
#define EXT4_NAME_LEN 255u

/* Group descriptor flags (ext4.h:445-448). */
#define EXT4_BG_INODE_UNINIT 0x0001u
#define EXT4_BG_BLOCK_UNINIT 0x0002u
#define EXT4_BG_INODE_ZEROED 0x0004u

/* Feature masks (ext4.h:2178-2233). */
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC 0x0001u
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES 0x0002u
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT4_FEATURE_COMPAT_EXT_ATTR 0x0008u
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010u
#define EXT4_FEATURE_COMPAT_DIR_INDEX 0x0020u
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200u
#define EXT4_FEATURE_COMPAT_FAST_COMMIT 0x0400u
#define EXT4_FEATURE_COMPAT_STABLE_INODES 0x0800u
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE 0x1000u

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR 0x0004u
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE 0x0008u
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM 0x0010u
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x0020u
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040u
#define EXT4_FEATURE_RO_COMPAT_QUOTA 0x0100u
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC 0x0200u
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400u
#define EXT4_FEATURE_RO_COMPAT_READONLY 0x1000u
#define EXT4_FEATURE_RO_COMPAT_PROJECT 0x2000u
#define EXT4_FEATURE_RO_COMPAT_VERITY 0x8000u
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000u

#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001u
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008u
#define EXT4_FEATURE_INCOMPAT_META_BG 0x0010u
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080u
#define EXT4_FEATURE_INCOMPAT_MMP 0x0100u
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200u
#define EXT4_FEATURE_INCOMPAT_EA_INODE 0x0400u
#define EXT4_FEATURE_INCOMPAT_DIRDATA 0x1000u
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000u
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x4000u
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000u
#define EXT4_FEATURE_INCOMPAT_ENCRYPT 0x10000u
#define EXT4_FEATURE_INCOMPAT_CASEFOLD 0x20000u

/* Parsed superblock: 64-bit counts are combined from the _lo/_hi words
 * only when feature_incompat has EXT4_FEATURE_INCOMPAT_64BIT. */
struct storage_ext4_super_view {
    uint64_t blocks_count;
    uint64_t reserved_blocks_count;
    uint64_t free_blocks_count;
    uint64_t group_count;
    uint32_t inodes_count;
    uint32_t free_inodes_count;
    uint32_t block_size;
    uint32_t cluster_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t desc_size;
    uint32_t inode_size;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t journal_inum;
    uint32_t checksum_seed; /* s_checksum_seed @0x270 (CSUM_SEED feature) */
    uint8_t uuid[16];
    char volume_name[17];
};

/* Parsed group descriptor.  The 64-bit bitmap/table and count fields are
 * combined from the _lo/_hi words only when desc_size >= 64.  The two
 * bitmap checksum words (bg_block_bitmap_csum / bg_inode_bitmap_csum) are
 * NOT parsed by storage_ext4_parse_group_desc: storage_ext4_read_group
 * fills them from the raw descriptor and storage_ext4_write_group stores
 * them back (lo+hi when desc_size covers the hi slot, else lo only and the
 * hi half reads back as zero).  They hold the checksums that
 * linux/fs/ext4/bitmap.c stores in the descriptor for the two bitmap
 * blocks; callers that modify a bitmap must recompute the matching word
 * before storage_ext4_write_group. */
struct storage_ext4_group_view {
    uint64_t block_bitmap;
    uint64_t inode_bitmap;
    uint64_t inode_table;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t used_dirs_count;
    uint32_t itable_unused;
    uint16_t flags;
    uint16_t checksum;
    uint32_t block_bitmap_csum;
    uint32_t inode_bitmap_csum;
};

/* Parsed extent tree node header (ext4_extents.h:78-84). */
struct storage_ext4_extent_header_view {
    uint16_t magic;
    uint16_t entries;
    uint16_t max_entries;
    uint16_t depth;
    uint32_t generation;
};

/* In-memory inode, never cast over disk bytes. blocks is in 512-byte sectors. */
struct ext4_inode_view {
    uint64_t size, blocks, file_acl;
    uint32_t uid, gid, flags, generation;
    uint32_t atime, ctime, mtime, dtime;
    uint16_t mode, links_count, extra_isize;
    uint8_t i_block_raw[60];
};
struct storage_ext4_map_result {
    uint64_t physical;
    uint32_t length; /* nonzero contiguous run, measured in filesystem blocks */
    bool hole, unwritten;
};
struct storage_volume;
struct storage_ext4_journal;
struct storage_ext4_handle {
    struct storage_volume *volume;
    uint32_t credits;
    bool active;
};
#define EXT4_JOURNAL_CREDITS 32u
#define EXT4_INDEX_FL 0x1000u
int storage_ext4_dir_hash(struct storage_volume *,const char *,unsigned,uint32_t *);
struct storage_ext4_dirent { uint64_t ino; uint8_t type, name_len; char name[256]; };
/* Per-volume ext4 geometry.  Cache, journal and statistics fields are
 * added by later tasks.  The super_view / backup_bgs / reserved_gdt_blocks /
 * flex_log_groups / fs_error fields are published by storage_ext4_mount()
 * from the raw superblock (task 5 allocator context):
 *  - super_view is the parsed superblock kept for the metadata checksum
 *    entry points (feature words, uuid and s_checksum_seed all feed the
 *    group/inode/bitmap checksums) and for free-count bookkeeping;
 *  - backup_bgs are s_backup_bgs[0..1] (only meaningful with
 *    COMPAT_SPARSE_SUPER2, which decides where superblock backups live);
 *  - reserved_gdt_blocks is s_reserved_gdt_blocks (counts as base metadata
 *    of every group that carries a superblock backup);
 *  - flex_log_groups is s_log_groups_per_flex clamped to
 *    EXT4_MAX_FLEX_LOG (mount logs the clamp; flex sizes above 2^7 would
 *    make the per-flex free-count sums explode);
 *  - fs_error is a sticky flag set when metadata checksum verification
 *    fails on an allocation path ("mark the volume errored"); later write
 *    paths must refuse to mutate a volume once it is set;
 *  - next_goal_block / reserved_window_end hold the allocator's per-volume
 *    goal hint and its reservation window (updated by every successful
 *    storage_ext4_alloc_blocks(), released when freed blocks fall inside
 *    the window). */
struct storage_ext4_state {
    uint64_t partition_start_lba;
    uint64_t partition_sector_count;
    uint64_t blocks_count;
    uint64_t group_count;
    uint32_t block_size;
    uint32_t cluster_size;
    uint32_t readahead_blocks;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inodes_count;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t desc_size;
    struct storage_ext4_super_view super_view;
    uint32_t reserved_gdt_blocks;
    uint32_t backup_bgs[2];
    uint8_t flex_log_groups;
    uint8_t fs_error;
    uint64_t next_goal_block;
    uint64_t reserved_window_end;
    uint64_t read_bytes, read_commands, read_blocks, read_cache_hits;
    struct storage_ext4_journal *journal;
    uint64_t journal_commits, journal_replays, journal_revoke_hits;
};
#define STORAGE_EXT4_READ_ONLY_NONE 0u
#define STORAGE_EXT4_READ_ONLY_READONLY_FEATURE 1u
#define STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT 2u
#define STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY 3u
#define STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT 4u

/** @brief Maximum number of parsed ext4 group descriptors cached by alloc.c. */
#ifndef EXT4_GROUP_CACHE_ENTRIES
#define EXT4_GROUP_CACHE_ENTRIES 64u
#endif

/** @brief Reservation window in blocks used by the ext4 allocator. */
#ifndef EXT4_RESERVATION_WINDOW_BLOCKS
#define EXT4_RESERVATION_WINDOW_BLOCKS 8u
#endif

/** @brief Largest supported ext4 flex group logarithm. */
#define EXT4_MAX_FLEX_LOG 7u
#define EXT4_GOOD_OLD_FIRST_INO 11u

enum storage_ext4_feature_verdict {
    STORAGE_EXT4_FEATURE_RW = 0,
    STORAGE_EXT4_FEATURE_RO = 1,
    STORAGE_EXT4_FEATURE_REJECT = 2,
};

struct storage_ext4_feature_decision {
    uint8_t verdict;
    uint32_t read_only_reason;
};

#endif /* RELIEFNT_EXT4_STORAGE_EXT4_TYPES_H */

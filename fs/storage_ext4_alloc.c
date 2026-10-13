/* ext4 group descriptors, block/inode bitmaps and the flex-group allocator
 * (task 5).  See storage_internal.h for the six-function contract.
 *
 * Structure (host-testable: separate translation unit, no kernel services
 * beyond the bounded caches and console_printf):
 *  - a bounded LRU cache of parsed group descriptors (EXT4_GROUP_CACHE_ENTRIES,
 *    stamped with volume->mount_generation so a remount can never serve
 *    stale summaries); misses read the descriptor through the block cache
 *    and verify its checksum;
 *  - bitmap access with linux/fs/ext4/bitmap.c checksums (metadata_csum
 *    only; stored in bg_block_bitmap_csum / bg_inode_bitmap_csum),
 *    uninit_bg first-touch initialization per linux/fs/ext4/balloc.c
 *    ext4_init_block_bitmap() and ialloc.c's inode bitmap equivalent;
 *  - goal/reservation allocation: a single scan per group, runs found near
 *    goal first (at goal, above, below), candidate flex groups chosen by
 *    their accumulated free-block counts, shorter runs returned when no
 *    full run exists, and a reservation window of
 *    EXT4_RESERVATION_WINDOW_BLOCKS blocks behind next_goal_block;
 *  - inode allocation with reserved inodes 1-10 kept out and Linux's
 *    bg_itable_unused bookkeeping.
 *
 * Every mutation updates the in-memory group summaries, the on-disk
 * superblock free counts and the cached descriptor/bitmap/superblock blocks
 * (storage_ext4_cache_mark_dirty); journal handles wrap these dirty
 * metadata updates through the task 7 transaction wrappers below. */
#include "../drivers/bootstrap/storage/storage_internal.h"

/* Group descriptor field offsets (ext4.h:402-432). */
#define AGD_BLOCK_BITMAP_LO 0x00
#define AGD_INODE_BITMAP_LO 0x04
#define AGD_INODE_TABLE_LO 0x08
#define AGD_FREE_BLOCKS_LO 0x0C
#define AGD_FREE_INODES_LO 0x0E
#define AGD_USED_DIRS_LO 0x10
#define AGD_FLAGS 0x12
#define AGD_BBM_CSUM_LO 0x18
#define AGD_IBM_CSUM_LO 0x1A
#define AGD_ITABLE_UNUSED_LO 0x1C
#define AGD_CHECKSUM 0x1E
#define AGD_BLOCK_BITMAP_HI 0x20
#define AGD_INODE_BITMAP_HI 0x24
#define AGD_INODE_TABLE_HI 0x28
#define AGD_FREE_BLOCKS_HI 0x2C
#define AGD_FREE_INODES_HI 0x2E
#define AGD_USED_DIRS_HI 0x30
#define AGD_ITABLE_UNUSED_HI 0x32
#define AGD_BBM_CSUM_HI 0x38
#define AGD_IBM_CSUM_HI 0x3A
/* EXT4_BG_*_BITMAP_CSUM_HI_END (ext4.h:429-433): the hi checksum slot
 * exists only when the descriptor reaches past it. */
#define AGD_BBM_CSUM_HI_END (AGD_BBM_CSUM_HI + 2u)
#define AGD_IBM_CSUM_HI_END (AGD_IBM_CSUM_HI + 2u)
/* 64-bit descriptor words only exist from 64 bytes on. */
#define AGD_HI_MIN_DESC_SIZE 64u

/* ext4_inode i_links_count (ext4.h:794). */
#define AGD_INO_LINKS 0x1A

/* Superblock free-count fields (ext4.h:1369). */
#define AGD_SB_FREE_BLOCKS_LO 0x0C
#define AGD_SB_FREE_BLOCKS_HI 0x158
#define AGD_SB_FREE_INODES 0x10

/* Scratch for descriptor assembly (kernel stacks stay small). */
static uint8_t ext4_alloc_desc[EXT4_MAX_DESC_SIZE];
static uint8_t ext4_alloc_block[EXT4_MAX_BLOCK_SIZE];

/* ---- bounded group-summary cache -------------------------------------- */
struct ext4_group_summary {
    struct storage_volume *volume;
    uint64_t group;
    uint64_t age;
    uint32_t volume_generation;
    uint8_t valid;
    struct storage_ext4_group_view view;
};

static struct ext4_group_summary ext4_group_summaries[EXT4_GROUP_CACHE_ENTRIES];
static uint64_t ext4_group_summary_clock;
void storage_ext4_invalidate_groups(struct storage_volume *v)
{
    for (unsigned i=0;i<EXT4_GROUP_CACHE_ENTRIES;i++)
        if (ext4_group_summaries[i].volume==v) ext4_group_summaries[i].valid=0;
}

static struct ext4_group_summary *ext4_summary_lookup(struct storage_volume *volume,
                                                      uint64_t group)
{
    for (uint32_t i = 0; i < EXT4_GROUP_CACHE_ENTRIES; ++i) {
        struct ext4_group_summary *entry = &ext4_group_summaries[i];
        if (entry->valid && entry->volume == volume && entry->group == group &&
            entry->volume_generation == volume->mount_generation) {
            entry->age = ++ext4_group_summary_clock;
            return entry;
        }
    }
    return NULL;
}

static struct ext4_group_summary *ext4_summary_slot(struct storage_volume *volume,
                                                    uint64_t group)
{
    struct ext4_group_summary *victim = NULL;

    for (uint32_t i = 0; i < EXT4_GROUP_CACHE_ENTRIES; ++i) {
        struct ext4_group_summary *entry = &ext4_group_summaries[i];
        if (!entry->valid) {
            victim = entry;
            break;
        }
        if (!victim || entry->age < victim->age)
            victim = entry;
    }
    if (!victim)
        return NULL;
    victim->valid = 1;
    victim->volume = volume;
    victim->group = group;
    victim->volume_generation = volume->mount_generation;
    victim->age = ++ext4_group_summary_clock;
    return victim;
}

static void ext4_summary_put(struct storage_volume *volume, uint64_t group,
                             const struct storage_ext4_group_view *view)
{
    struct ext4_group_summary *entry = ext4_summary_lookup(volume, group);

    if (!entry)
        entry = ext4_summary_slot(volume, group);
    if (!entry)
        return; /* bounded cache full of live entries: refetch next time */
    entry->view = *view;
}

/* ---- geometry --------------------------------------------------------- */
static int ext4_geometry_ok(const struct storage_volume *volume)
{
    return volume && volume->ext4.blocks_count != 0 &&
           volume->ext4.group_count != 0 &&
           (volume->ext4.block_size == EXT4_MIN_BLOCK_SIZE ||
            volume->ext4.block_size == 2048 ||
            volume->ext4.block_size == EXT4_MAX_BLOCK_SIZE) &&
           volume->ext4.blocks_per_group != 0 &&
           volume->ext4.inodes_per_group != 0 &&
           volume->ext4.desc_size >= EXT4_MIN_DESC_SIZE &&
           volume->ext4.desc_size <= EXT4_MAX_DESC_SIZE;
}

static uint64_t ext4_group_first_block(const struct storage_volume *volume,
                                       uint64_t group)
{
    return (uint64_t)volume->ext4.first_data_block +
           group * (uint64_t)volume->ext4.blocks_per_group;
}

static uint32_t ext4_group_block_count(const struct storage_volume *volume,
                                       uint64_t group)
{
    uint64_t first = ext4_group_first_block(volume, group);
    uint64_t rest = volume->ext4.blocks_count - first;

    return (uint32_t)(rest < volume->ext4.blocks_per_group
                          ? rest
                          : volume->ext4.blocks_per_group);
}

static uint64_t ext4_group_of_block(const struct storage_volume *volume,
                                    uint64_t block)
{
    return (block - (uint64_t)volume->ext4.first_data_block) /
           volume->ext4.blocks_per_group;
}

static uint32_t ext4_group_itb(const struct storage_volume *volume)
{
    return (uint32_t)(((uint64_t)volume->ext4.inodes_per_group *
                       volume->ext4.inode_size) /
                      volume->ext4.block_size);
}

static uint8_t ext4_flex_log(const struct storage_volume *volume)
{
    return volume->ext4.flex_log_groups > EXT4_MAX_FLEX_LOG
               ? (uint8_t)EXT4_MAX_FLEX_LOG
               : volume->ext4.flex_log_groups;
}

static uint32_t ext4_flex_size(const struct storage_volume *volume)
{
    return 1u << ext4_flex_log(volume);
}

static uint64_t ext4_flex_of_group(const struct storage_volume *volume,
                                   uint64_t group)
{
    return group >> ext4_flex_log(volume);
}

static uint32_t ext4_gdt_blocks(const struct storage_volume *volume)
{
    uint64_t bytes = volume->ext4.group_count * (uint64_t)volume->ext4.desc_size;

    return (uint32_t)((bytes + volume->ext4.block_size - 1) /
                      volume->ext4.block_size);
}

/* ext4_bg_has_super() (balloc.c:858-876). */
static int ext4_test_root(uint32_t a, uint32_t b)
{
    while (a % b == 0) {
        a /= b;
        if (a == 1)
            return 1;
    }
    return 0;
}

static int ext4_bg_has_super(const struct storage_volume *volume, uint64_t group)
{
    const struct storage_ext4_super_view *view = &volume->ext4.super_view;

    if (group == 0)
        return 1;
    if ((view->feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) != 0)
        return group == volume->ext4.backup_bgs[0] ||
               group == volume->ext4.backup_bgs[1];
    if (group <= 1 ||
        (view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0)
        return 1;
    return ext4_test_root((uint32_t)group, 3) ||
           ext4_test_root((uint32_t)group, 5) ||
           ext4_test_root((uint32_t)group, 7);
}

/* ext4_num_base_meta_blocks() (balloc.c:935-951) without meta_bg (rejected
 * by the feature policy): superblock backup + group descriptor table +
 * reserved descriptor table blocks. */
static uint32_t ext4_base_meta_blocks(const struct storage_volume *volume,
                                      uint64_t group)
{
    if (!ext4_bg_has_super(volume, group))
        return 0;
    return 1u + ext4_gdt_blocks(volume) + volume->ext4.reserved_gdt_blocks;
}

/* ---- bitmap checksums (linux/fs/ext4/bitmap.c) ------------------------ */
static int ext4_metadata_csum(const struct storage_volume *volume)
{
    return (volume->ext4.super_view.feature_ro_compat &
            EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
}

static uint32_t ext4_bitmap_csum(const struct storage_volume *volume,
                                 const uint8_t *bitmap, uint32_t size)
{
    return storage_ext4_crc32c(storage_ext4_super_csum_seed(&volume->ext4.super_view),
                               bitmap, size);
}

/* size is EXT4_CLUSTERS_PER_GROUP/8 for the block bitmap and
 * EXT4_INODES_PER_GROUP/8 for the inode bitmap (bigalloc is rejected by
 * the feature policy, so clusters == blocks). */
static int ext4_bitmap_csum_verify(const struct storage_volume *volume,
                                   const uint8_t *bitmap, uint32_t size,
                                   uint32_t provided, int inode_bitmap)
{
    uint32_t calculated;

    if (!ext4_metadata_csum(volume))
        return 0;
    calculated = ext4_bitmap_csum(volume, bitmap, size);
    if (inode_bitmap) {
        if (volume->ext4.desc_size >= AGD_IBM_CSUM_HI_END)
            return provided == calculated ? 0 : -RELIEFOS_EIO;
        return ((uint16_t)provided == (uint16_t)calculated) ? 0 : -RELIEFOS_EIO;
    }
    if (volume->ext4.desc_size >= AGD_BBM_CSUM_HI_END)
        return provided == calculated ? 0 : -RELIEFOS_EIO;
    return ((uint16_t)provided == (uint16_t)calculated) ? 0 : -RELIEFOS_EIO;
}

static void ext4_bitmap_csum_set(const struct storage_volume *volume,
                                 const uint8_t *bitmap, uint32_t size,
                                 struct storage_ext4_group_view *view,
                                 int inode_bitmap)
{
    uint32_t csum;

    /* Only metadata_csum filesystems carry bitmap checksums; on a
     * gdt_csum (uninit_bg-only) volume the descriptor words stay as they
     * are on disk. */
    if (!ext4_metadata_csum(volume))
        return;
    csum = ext4_bitmap_csum(volume, bitmap, size);
    if (inode_bitmap)
        view->inode_bitmap_csum = csum;
    else
        view->block_bitmap_csum = csum;
}

static void ext4_mark_volume_error(struct storage_volume *volume, uint64_t group,
                                   int inode_bitmap)
{
    if (!volume->ext4.fs_error) {
        console_printf("[reliefnt] ext4 %s bitmap checksum error group=%llu\n",
                       inode_bitmap ? "inode" : "block",
                       (unsigned long long)group);
    }
    volume->ext4.fs_error = 1;
}

/* ---- group descriptor read/write -------------------------------------- */
static void ext4_gd_address(const struct storage_volume *volume, uint64_t group,
                            uint64_t *block, uint32_t *in_block)
{
    uint64_t offset = group * (uint64_t)volume->ext4.desc_size;
    uint64_t gdt = (uint64_t)volume->ext4.first_data_block + 1u;

    *block = gdt + offset / volume->ext4.block_size;
    *in_block = (uint32_t)(offset % volume->ext4.block_size);
}

/* Raw descriptor read: assembles the descriptor (it may straddle two
 * blocks), verifies its checksum and parses it. */
static int ext4_read_group_disk(struct storage_volume *volume, uint64_t group,
                                struct storage_ext4_group_view *view)
{
    uint64_t block;
    uint32_t in_block;
    uint8_t *desc = ext4_alloc_desc;
    int ret;

    ext4_gd_address(volume, group, &block, &in_block);
    if (in_block + volume->ext4.desc_size <= volume->ext4.block_size) {
        ret = storage_ext4_cache_read(volume, block, ext4_alloc_block);
        if (ret < 0)
            return ret;
        storage_memcpy(desc, ext4_alloc_block + in_block, volume->ext4.desc_size);
    } else {
        uint32_t first = volume->ext4.block_size - in_block;
        ret = storage_ext4_cache_read(volume, block, ext4_alloc_block);
        if (ret < 0)
            return ret;
        storage_memcpy(desc, ext4_alloc_block + in_block, first);
        ret = storage_ext4_cache_read(volume, block + 1u, ext4_alloc_block);
        if (ret < 0)
            return ret;
        storage_memcpy(desc + first, ext4_alloc_block,
                       volume->ext4.desc_size - first);
    }
    ret = storage_ext4_verify_group_checksum(desc, volume->ext4.desc_size,
                                             (uint32_t)group,
                                             &volume->ext4.super_view);
    if (ret < 0)
        return ret;
    storage_memzero(view, sizeof(*view));
    ret = storage_ext4_parse_group_desc(desc, volume->ext4.desc_size,
                                        volume->ext4.desc_size, view);
    if (ret < 0)
        return ret;
    /* Bitmap checksum words: lo always, hi only where the slot exists
     * (bitmap.c verify semantics). */
    view->block_bitmap_csum = ext4_get_le16(desc + AGD_BBM_CSUM_LO);
    view->inode_bitmap_csum = ext4_get_le16(desc + AGD_IBM_CSUM_LO);
    if (volume->ext4.desc_size >= AGD_BBM_CSUM_HI_END)
        view->block_bitmap_csum |=
            (uint32_t)ext4_get_le16(desc + AGD_BBM_CSUM_HI) << 16;
    if (volume->ext4.desc_size >= AGD_IBM_CSUM_HI_END)
        view->inode_bitmap_csum |=
            (uint32_t)ext4_get_le16(desc + AGD_IBM_CSUM_HI) << 16;
    return 0;
}

/* Summary-cached descriptor read (storage_ext4_read_group body). */
static int ext4_group_summary(struct storage_volume *volume, uint64_t group,
                              struct storage_ext4_group_view *view)
{
    struct ext4_group_summary *entry = ext4_summary_lookup(volume, group);
    int ret;

    if (entry) {
        *view = entry->view;
        return 0;
    }
    ret = ext4_read_group_disk(volume, group, view);
    if (ret < 0)
        return ret;
    ext4_summary_put(volume, group, view);
    return 0;
}

int storage_ext4_read_group(struct storage_volume *volume, uint64_t group,
                            struct storage_ext4_group_view *out)
{
    if (!out || !ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    if (group >= volume->ext4.group_count)
        return -RELIEFOS_EINVAL;
    return ext4_group_summary(volume, group, out);
}

int storage_ext4_write_group(struct storage_volume *volume, uint64_t group,
                             const struct storage_ext4_group_view *view)
{
    uint64_t block;
    uint32_t in_block;
    uint32_t first;
    uint8_t *desc = ext4_alloc_desc;
    uint8_t *first_data;
    uint8_t *second_data = NULL;
    bool straddle;
    int ret;

    if (!view || !ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    if (group >= volume->ext4.group_count)
        return -RELIEFOS_EINVAL;

    /* Both block pins are held until the single release path below, so no
     * failure can leak one of them. */
    ext4_gd_address(volume, group, &block, &in_block);
    straddle = in_block + volume->ext4.desc_size > volume->ext4.block_size;
    ret = storage_ext4_cache_get(volume, block, &first_data, true);
    if (ret < 0)
        return ret;
    if (straddle) {
        ret = storage_ext4_cache_get(volume, block + 1u, &second_data, true);
        if (ret < 0) {
            (void)storage_ext4_cache_mark_dirty(volume, block);
            return ret;
        }
        first = volume->ext4.block_size - in_block;
        storage_memcpy(desc, first_data + in_block, first);
        storage_memcpy(desc + first, second_data,
                       volume->ext4.desc_size - first);
    } else {
        storage_memcpy(desc, first_data + in_block, volume->ext4.desc_size);
    }

    /* Only the fields the view carries are rewritten; everything else
     * (exclude bitmap, reserved bytes) keeps its on-disk value. */
    ext4_put_le32(desc + AGD_BLOCK_BITMAP_LO, (uint32_t)view->block_bitmap);
    ext4_put_le32(desc + AGD_INODE_BITMAP_LO, (uint32_t)view->inode_bitmap);
    ext4_put_le32(desc + AGD_INODE_TABLE_LO, (uint32_t)view->inode_table);
    ext4_put_le16(desc + AGD_FREE_BLOCKS_LO,
                  (uint16_t)view->free_blocks_count);
    ext4_put_le16(desc + AGD_FREE_INODES_LO,
                  (uint16_t)view->free_inodes_count);
    ext4_put_le16(desc + AGD_USED_DIRS_LO, (uint16_t)view->used_dirs_count);
    ext4_put_le16(desc + AGD_FLAGS, view->flags);
    ext4_put_le16(desc + AGD_BBM_CSUM_LO,
                  (uint16_t)(view->block_bitmap_csum & 0xffffu));
    ext4_put_le16(desc + AGD_IBM_CSUM_LO,
                  (uint16_t)(view->inode_bitmap_csum & 0xffffu));
    ext4_put_le16(desc + AGD_ITABLE_UNUSED_LO, (uint16_t)view->itable_unused);
    if (volume->ext4.desc_size >= AGD_HI_MIN_DESC_SIZE) {
        ext4_put_le32(desc + AGD_BLOCK_BITMAP_HI,
                      (uint32_t)(view->block_bitmap >> 32));
        ext4_put_le32(desc + AGD_INODE_BITMAP_HI,
                      (uint32_t)(view->inode_bitmap >> 32));
        ext4_put_le32(desc + AGD_INODE_TABLE_HI,
                      (uint32_t)(view->inode_table >> 32));
        ext4_put_le16(desc + AGD_FREE_BLOCKS_HI,
                      (uint16_t)(view->free_blocks_count >> 16));
        ext4_put_le16(desc + AGD_FREE_INODES_HI,
                      (uint16_t)(view->free_inodes_count >> 16));
        ext4_put_le16(desc + AGD_USED_DIRS_HI,
                      (uint16_t)(view->used_dirs_count >> 16));
        ext4_put_le16(desc + AGD_ITABLE_UNUSED_HI,
                      (uint16_t)(view->itable_unused >> 16));
    }
    if (volume->ext4.desc_size >= AGD_BBM_CSUM_HI_END)
        ext4_put_le16(desc + AGD_BBM_CSUM_HI,
                      (uint16_t)(view->block_bitmap_csum >> 16));
    if (volume->ext4.desc_size >= AGD_IBM_CSUM_HI_END)
        ext4_put_le16(desc + AGD_IBM_CSUM_HI,
                      (uint16_t)(view->inode_bitmap_csum >> 16));
    ret = storage_ext4_update_group_checksum(desc, volume->ext4.desc_size,
                                             (uint32_t)group,
                                             &volume->ext4.super_view);
    if (ret == 0) {
        if (straddle) {
            storage_memcpy(first_data + in_block, desc, first);
            storage_memcpy(second_data, desc + first,
                           volume->ext4.desc_size - first);
        } else {
            storage_memcpy(first_data + in_block, desc,
                           volume->ext4.desc_size);
        }
    }
    /* journal handle: task 7 */
    /* Every exit releases both pins: on success this publishes the new
     * descriptor, on failure the untouched buffers. */
    {
        int release1 = storage_ext4_cache_mark_dirty(volume, block);
        int release2 = straddle
                           ? storage_ext4_cache_mark_dirty(volume, block + 1u)
                           : 0;
        if (ret == 0)
            ret = release1 != 0 ? release1 : release2;
    }
    if (ret < 0)
        return ret;
    ext4_summary_put(volume, group, view);
    return 0;
}

/* ---- superblock free counts ------------------------------------------- */
static void ext4_super_block_address(const struct storage_volume *volume,
                                     uint64_t *block, uint32_t *offset)
{
    *block = EXT4_SUPERBLOCK_OFFSET / volume->ext4.block_size;
    *offset = EXT4_SUPERBLOCK_OFFSET % volume->ext4.block_size;
}

static int ext4_super_counts_update(struct storage_volume *volume,
                                    int64_t delta_blocks, int64_t delta_inodes)
{
    uint64_t block;
    uint32_t offset;
    uint8_t *data;
    uint64_t free_blocks;
    uint32_t free_inodes;
    int ret;

    ext4_super_block_address(volume, &block, &offset);
    ret = storage_ext4_cache_get(volume, block, &data, true);
    if (ret < 0)
        return ret;
    free_blocks = ext4_get_le32(data + offset + AGD_SB_FREE_BLOCKS_LO);
    if (volume->ext4.super_view.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        free_blocks |= (uint64_t)ext4_get_le32(data + offset +
                                               AGD_SB_FREE_BLOCKS_HI)
                       << 32;
    free_inodes = ext4_get_le32(data + offset + AGD_SB_FREE_INODES);
    if (delta_blocks < 0 && (uint64_t)(-delta_blocks) > free_blocks) {
        free_blocks = 0;
        volume->ext4.fs_error = 1;
    } else {
        free_blocks = (uint64_t)((int64_t)free_blocks + delta_blocks);
    }
    if (delta_inodes < 0 && (uint32_t)(-delta_inodes) > free_inodes) {
        free_inodes = 0;
        volume->ext4.fs_error = 1;
    } else {
        free_inodes = (uint32_t)((int64_t)free_inodes + delta_inodes);
    }
    ext4_put_le32(data + offset + AGD_SB_FREE_BLOCKS_LO,
                  (uint32_t)(free_blocks & 0xffffffffu));
    if (volume->ext4.super_view.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        ext4_put_le32(data + offset + AGD_SB_FREE_BLOCKS_HI,
                      (uint32_t)(free_blocks >> 32));
    ext4_put_le32(data + offset + AGD_SB_FREE_INODES, free_inodes);
    ret = storage_ext4_update_super_checksum(data + offset,
                                             EXT4_SUPERBLOCK_SIZE);
    if (ret < 0) {
        (void)storage_ext4_cache_mark_dirty(volume, block);
        return ret;
    }
    /* journal handle: task 7 */
    ret = storage_ext4_cache_mark_dirty(volume, block);
    if (ret < 0)
        return ret;
    volume->ext4.super_view.free_blocks_count = free_blocks;
    volume->ext4.super_view.free_inodes_count = free_inodes;
    return 0;
}

/* ---- uninit bitmap initialization ------------------------------------- */
static int ext4_block_in_group(const struct storage_volume *volume, uint64_t block,
                               uint64_t group)
{
    uint64_t first = ext4_group_first_block(volume, group);

    return block >= first &&
           block < first + ext4_group_block_count(volume, group);
}

static void ext4_set_bit(uint8_t *bitmap, uint32_t bit)
{ bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u)); }

static int ext4_test_bit(const uint8_t *bitmap, uint32_t bit)
{ return (bitmap[bit / 8u] >> (bit % 8u)) & 1u; }

/* ext4_init_block_bitmap() (balloc.c:182-233): mark the in-group metadata
 * and everything past the end of the filesystem used, then recompute the
 * free count from the bitmap. */
static int ext4_init_block_bitmap(struct storage_volume *volume, uint64_t group,
                                  struct storage_ext4_group_view *view)
{
    uint8_t *bitmap;
    uint32_t blocks = ext4_group_block_count(volume, group);
    uint32_t base = ext4_base_meta_blocks(volume, group);
    uint32_t free_count = 0;
    uint64_t first = ext4_group_first_block(volume, group);
    int ret;

    ret = storage_ext4_cache_get(volume, view->block_bitmap, &bitmap, true);
    if (ret < 0)
        return ret;
    storage_memzero(bitmap, volume->ext4.block_size);
    for (uint32_t bit = 0; bit < base && bit < blocks; ++bit)
        ext4_set_bit(bitmap, bit);
    if (ext4_block_in_group(volume, view->block_bitmap, group))
        ext4_set_bit(bitmap, (uint32_t)(view->block_bitmap - first));
    if (ext4_block_in_group(volume, view->inode_bitmap, group))
        ext4_set_bit(bitmap, (uint32_t)(view->inode_bitmap - first));
    for (uint32_t i = 0; i < ext4_group_itb(volume); ++i) {
        if (ext4_block_in_group(volume, view->inode_table + i, group))
            ext4_set_bit(bitmap, (uint32_t)(view->inode_table + i - first));
    }
    for (uint32_t bit = blocks; bit < volume->ext4.block_size * 8u; ++bit)
        ext4_set_bit(bitmap, bit);
    for (uint32_t bit = 0; bit < blocks; ++bit) {
        if (!ext4_test_bit(bitmap, bit))
            ++free_count;
    }
    view->free_blocks_count = free_count;
    view->flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
    ext4_bitmap_csum_set(volume, bitmap, volume->ext4.blocks_per_group / 8u,
                         view, 0);
    ret = storage_ext4_write_group(volume, group, view);
    if (ret < 0) {
        (void)storage_ext4_cache_mark_dirty(volume, view->block_bitmap);
        return ret;
    }
    /* journal handle: task 7 */
    return storage_ext4_cache_mark_dirty(volume, view->block_bitmap);
}

/* Inode-bitmap equivalent: bits past s_inodes_count and the bitmap padding
 * are set, bg_itable_unused covers the whole unused table, the flag goes. */
static int ext4_init_inode_bitmap(struct storage_volume *volume, uint64_t group,
                                  struct storage_ext4_group_view *view)
{
    uint8_t *bitmap;
    uint32_t inodes = volume->ext4.inodes_per_group;
    uint64_t group_ino = group * (uint64_t)inodes;
    uint32_t valid = inodes;
    uint32_t free_count = 0;
    int ret;

    if (group_ino + valid > volume->ext4.inodes_count)
        valid = (uint32_t)(volume->ext4.inodes_count - group_ino);
    ret = storage_ext4_cache_get(volume, view->inode_bitmap, &bitmap, true);
    if (ret < 0)
        return ret;
    storage_memzero(bitmap, volume->ext4.block_size);
    for (uint32_t bit = valid; bit < volume->ext4.block_size * 8u; ++bit)
        ext4_set_bit(bitmap, bit);
    for (uint32_t bit = 0; bit < valid; ++bit) {
        if (!ext4_test_bit(bitmap, bit))
            ++free_count;
    }
    view->free_inodes_count = free_count;
    view->itable_unused = inodes;
    view->flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
    ext4_bitmap_csum_set(volume, bitmap, inodes / 8u, view, 1);
    ret = storage_ext4_write_group(volume, group, view);
    if (ret < 0) {
        (void)storage_ext4_cache_mark_dirty(volume, view->inode_bitmap);
        return ret;
    }
    /* journal handle: task 7 */
    return storage_ext4_cache_mark_dirty(volume, view->inode_bitmap);
}

/* ---- bitmap scanning --------------------------------------------------- */
struct ext4_scan_result {
    int64_t hit;        /* run covers [hint, hint+need): allocate at hint */
    int64_t above;      /* first full run starting above hint */
    int64_t below;      /* last full run starting below hint */
    int64_t best_start; /* longest free run seen */
    uint32_t best_len;
};

static int ext4_bit_free(const struct storage_volume *volume, const uint8_t *bitmap,
                         uint64_t block, uint32_t bit, bool window_owner)
{
    if (ext4_test_bit(bitmap, bit))
        return 0;
    if (!window_owner && block >= volume->ext4.next_goal_block &&
        block < volume->ext4.reserved_window_end)
        return 0;
    return 1;
}

static void ext4_scan_run(struct ext4_scan_result *out, uint32_t start,
                          uint32_t len, uint64_t hint, uint32_t need)
{
    uint64_t end;

    if (len == 0)
        return;
    if (len > out->best_len) {
        out->best_len = len;
        out->best_start = start;
    }
    if (len < need)
        return;
    end = (uint64_t)start + len;
    if ((uint64_t)start <= hint && hint + (uint64_t)need <= end) {
        if (out->hit < 0)
            out->hit = (int64_t)hint;
    } else if ((uint64_t)start > hint) {
        if (out->above < 0)
            out->above = start;
    } else {
        out->below = start; /* keep the run closest to hint */
    }
}

static void ext4_scan_bits(const struct storage_volume *volume,
                           const uint8_t *bitmap, uint64_t first_block,
                           uint32_t lo, uint32_t hi, uint64_t hint, uint32_t need,
                           bool window_owner, struct ext4_scan_result *out)
{
    uint32_t run_start = 0;
    uint32_t run_len = 0;

    for (uint32_t bit = lo; bit < hi; ++bit) {
        if (ext4_bit_free(volume, bitmap, first_block + bit, bit,
                          window_owner)) {
            if (run_len == 0)
                run_start = bit;
            ++run_len;
            continue;
        }
        ext4_scan_run(out, run_start, run_len, hint, need);
        run_len = 0;
    }
    ext4_scan_run(out, run_start, run_len, hint, need);
}

/* Length of the free run touching bit `from` walking towards `step`. */
static uint32_t ext4_run_touching(const struct storage_volume *volume,
                                  const uint8_t *bitmap, uint64_t first_block,
                                  uint32_t from, uint32_t limit, int step,
                                  bool window_owner)
{
    uint32_t len = 0;
    uint32_t bit = from;

    for (;;) {
        if (!ext4_bit_free(volume, bitmap, first_block + bit, bit, window_owner))
            break;
        ++len;
        if (step > 0) {
            if (bit + 1 >= limit)
                break;
            ++bit;
        } else {
            if (bit == 0)
                break;
            --bit;
        }
    }
    return len;
}

/* Load a group's block bitmap (initializing it on first touch when
 * BLOCK_UNINIT is set) and verify its checksum. */
static int ext4_load_block_bitmap(struct storage_volume *volume, uint64_t group,
                                  struct storage_ext4_group_view *view,
                                  uint8_t **bitmap, bool for_write)
{
    int ret;

    ret = ext4_group_summary(volume, group, view);
    if (ret < 0)
        return ret;
    if ((view->flags & EXT4_BG_BLOCK_UNINIT) != 0) {
        ret = ext4_init_block_bitmap(volume, group, view);
        if (ret < 0)
            return ret;
    }
    ret = storage_ext4_cache_get(volume, view->block_bitmap, bitmap, for_write);
    if (ret < 0)
        return ret;
    ret = ext4_bitmap_csum_verify(volume, *bitmap,
                                  volume->ext4.blocks_per_group / 8u,
                                  view->block_bitmap_csum, 0);
    if (ret < 0) {
        /* A for_write get pins the entry; release it on the way out
         * (the data is untouched, the write-back is a no-op). */
        if (for_write)
            (void)storage_ext4_cache_mark_dirty(volume, view->block_bitmap);
        ext4_mark_volume_error(volume, group, 0);
        return ret;
    }
    return 0;
}

static int ext4_load_inode_bitmap(struct storage_volume *volume, uint64_t group,
                                  struct storage_ext4_group_view *view,
                                  uint8_t **bitmap, bool for_write)
{
    int ret;

    ret = ext4_group_summary(volume, group, view);
    if (ret < 0)
        return ret;
    if ((view->flags & EXT4_BG_INODE_UNINIT) != 0) {
        if (group == 0) {
            /* linux/fs/ext4/ialloc.c refuses INODE_UNINIT on group 0. */
            ext4_mark_volume_error(volume, group, 1);
            return -RELIEFOS_EIO;
        }
        ret = ext4_init_inode_bitmap(volume, group, view);
        if (ret < 0)
            return ret;
    }
    ret = storage_ext4_cache_get(volume, view->inode_bitmap, bitmap, for_write);
    if (ret < 0)
        return ret;
    ret = ext4_bitmap_csum_verify(volume, *bitmap,
                                  volume->ext4.inodes_per_group / 8u,
                                  view->inode_bitmap_csum, 1);
    if (ret < 0) {
        if (for_write)
            (void)storage_ext4_cache_mark_dirty(volume, view->inode_bitmap);
        ext4_mark_volume_error(volume, group, 1);
        return ret;
    }
    return 0;
}

/* ---- run allocation ---------------------------------------------------- */
struct ext4_run {
    uint64_t first;
    uint32_t len;
};

static int ext4_apply_chunk(struct storage_volume *volume, uint64_t group,
                            uint32_t offset, uint32_t len, bool allocate)
{
    struct storage_ext4_group_view view;
    uint8_t *bitmap;
    int ret;

    /* The count guard runs before any write pin is taken or any bit is
     * touched: a failing chunk must leave neither a pinned slot nor a
     * half-set bitmap behind. */
    ret = ext4_group_summary(volume, group, &view);
    if (ret < 0)
        return ret;
    if (allocate && view.free_blocks_count < len) {
        volume->ext4.fs_error = 1;
        return -RELIEFOS_EIO;
    }
    ret = ext4_load_block_bitmap(volume, group, &view, &bitmap, true);
    if (ret < 0)
        return ret;
    for (uint32_t i = 0; i < len; ++i) {
        if (allocate)
            ext4_set_bit(bitmap, offset + i);
        else
            bitmap[(offset + i) / 8u] &=
                (uint8_t)~(1u << ((offset + i) % 8u));
    }
    if (allocate)
        view.free_blocks_count -= len;
    else
        view.free_blocks_count += len;
    ext4_bitmap_csum_set(volume, bitmap, volume->ext4.blocks_per_group / 8u,
                         &view, 0);
    ret = storage_ext4_write_group(volume, group, &view);
    if (ret < 0) {
        /* Release the pin either way; the caller rolls the change back. */
        (void)storage_ext4_cache_mark_dirty(volume, view.block_bitmap);
        return ret;
    }
    /* journal handle: task 7 */
    return storage_ext4_cache_mark_dirty(volume, view.block_bitmap);
}

/* Apply [first, first+len) group by group; *done reports how far it got. */
static int ext4_apply_range(struct storage_volume *volume, uint64_t first,
                            uint32_t len, bool allocate, uint32_t *done)
{
    *done = 0;
    while (*done < len) {
        uint64_t block = first + *done;
        uint64_t group = ext4_group_of_block(volume, block);
        uint64_t group_end = ext4_group_first_block(volume, group) +
                             ext4_group_block_count(volume, group);
        uint32_t chunk = len - *done;
        int ret;

        if (block + chunk > group_end)
            chunk = (uint32_t)(group_end - block);
        ret = ext4_apply_chunk(volume, group,
                               (uint32_t)(block -
                                          ext4_group_first_block(volume, group)),
                               chunk, allocate);
        if (ret < 0)
            return ret;
        *done += chunk;
    }
    return 0;
}

/* Commit [first, first+len) with all-or-nothing failure behaviour: a
 * partial run is rolled back with the inverse operation before the error
 * is returned (best effort, the error of the original failure wins). */
static int ext4_commit_run(struct storage_volume *volume, uint64_t first,
                           uint32_t len, bool allocate)
{
    uint32_t done = 0;
    uint32_t ignored;
    int ret = ext4_apply_range(volume, first, len, allocate, &done);

    if (ret < 0) {
        /* Undo the chunks that landed; the superblock counts were never
         * touched on this path, so they must NOT be compensated here
         * (that used to drift s_free_blocks_count by `done`). */
        (void)ext4_apply_range(volume, first, done, !allocate, &ignored);
        return ret;
    }
    ret = ext4_super_counts_update(volume, allocate ? -(int64_t)len
                                                    : (int64_t)len,
                                   0);
    if (ret < 0) {
        (void)ext4_apply_range(volume, first, len, !allocate, &ignored);
        return ret;
    }
    return 0;
}

/* Search one group for a run of `need` blocks; cross-group runs may extend
 * past the group boundary when scanning near goal. */
static int ext4_scan_group(struct storage_volume *volume, uint64_t group,
                           uint64_t hint, uint32_t need, bool extend,
                           bool window_owner, struct ext4_run *best)
{
    struct storage_ext4_group_view view;
    struct ext4_scan_result scan;
    uint8_t *bitmap;
    uint32_t blocks;
    uint64_t first;
    int ret;

    ret = ext4_group_summary(volume, group, &view);
    if (ret < 0)
        return ret;
    if (view.free_blocks_count == 0)
        return 0;
    blocks = ext4_group_block_count(volume, group);
    first = ext4_group_first_block(volume, group);
    ret = ext4_load_block_bitmap(volume, group, &view, &bitmap, false);
    if (ret < 0)
        return ret;

    scan.hit = scan.above = scan.below = -1;
    scan.best_start = -1;
    scan.best_len = 0;
    ext4_scan_bits(volume, bitmap, first, 0, blocks, hint, need, window_owner,
                   &scan);
    if (scan.hit >= 0 || scan.above >= 0 || scan.below >= 0) {
        best->first = first + (uint64_t)(scan.hit >= 0
                                             ? scan.hit
                                             : (scan.above >= 0 ? scan.above
                                                                : scan.below));
        best->len = need;
        return 1;
    }

    /* Cross-group extension near goal: the run at the tail of this group
     * may continue with the head of the next one (contiguous block
     * numbers). */
    if (extend && blocks == volume->ext4.blocks_per_group &&
        group + 1 < volume->ext4.group_count) {
        uint32_t tail = ext4_run_touching(volume, bitmap, first, blocks - 1,
                                          blocks, -1, window_owner);
        if (tail > 0) {
            struct storage_ext4_group_view next_view;
            uint8_t *next_bitmap;
            uint32_t next_blocks;
            uint32_t head;
            uint32_t combined;

            ret = ext4_load_block_bitmap(volume, group + 1, &next_view,
                                         &next_bitmap, false);
            if (ret < 0)
                return ret;
            next_blocks = ext4_group_block_count(volume, group + 1);
            head = ext4_run_touching(volume, next_bitmap,
                                     ext4_group_first_block(volume, group + 1),
                                     0, next_blocks, 1, window_owner);
            combined = tail + head;
            if (combined >= need) {
                best->first = first + (blocks - tail);
                best->len = need;
                return 1;
            }
            if (combined > best->len && combined > scan.best_len) {
                scan.best_len = combined;
                scan.best_start = (int64_t)(blocks - tail);
            }
        }
    }
    if (scan.best_len > best->len) {
        best->len = scan.best_len;
        best->first = first + (uint64_t)scan.best_start;
    }
    return 0;
}

/* Free-block sum of one flex group (on-demand member accumulation). */
static int ext4_flex_free_blocks(struct storage_volume *volume, uint64_t flex,
                                 uint64_t *out)
{
    uint32_t size = ext4_flex_size(volume);
    uint64_t first_member = flex * (uint64_t)size;
    uint64_t last = first_member + size;
    uint64_t total = 0;

    if (last > volume->ext4.group_count)
        last = volume->ext4.group_count;
    for (uint64_t g = first_member; g < last; ++g) {
        struct storage_ext4_group_view view;
        int ret = ext4_group_summary(volume, g, &view);

        if (ret < 0)
            return ret;
        total += view.free_blocks_count;
    }
    *out = total;
    return 0;
}

static int ext4_alloc_blocks_impl(struct storage_volume *volume, uint64_t goal,
                              uint32_t count, uint64_t *first,
                              uint32_t *allocated)
{
    struct ext4_run best = {0, 0};
    uint64_t goal_group, goal_flex, flex_count;
    uint32_t hint;
    bool window_owner;
    int ret;

    if (!first || !allocated || !ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    *first = 0;
    *allocated = 0;
    if (count == 0 || goal >= volume->ext4.blocks_count)
        return -RELIEFOS_EINVAL;
    if (goal < volume->ext4.first_data_block)
        goal = volume->ext4.first_data_block;

    window_owner = volume->ext4.reserved_window_end >
                           volume->ext4.next_goal_block &&
                       goal >= volume->ext4.next_goal_block &&
                       goal < volume->ext4.reserved_window_end;
    goal_group = ext4_group_of_block(volume, goal);
    goal_flex = ext4_flex_of_group(volume, goal_group);
    flex_count = (volume->ext4.group_count + ext4_flex_size(volume) - 1) /
                 ext4_flex_size(volume);
    hint = (uint32_t)(goal - ext4_group_first_block(volume, goal_group));

    /* Phase 1: goal's group, near goal first (forward and backward). */
    ret = ext4_scan_group(volume, goal_group, hint, count, true, window_owner,
                          &best);
    if (ret < 0)
        return ret;
    if (ret == 1)
        goto commit;

    /* Phase 2: the remaining members of goal's flex group. */
    {
        uint64_t first_member = goal_flex * (uint64_t)ext4_flex_size(volume);
        uint64_t last = first_member + ext4_flex_size(volume);

        if (last > volume->ext4.group_count)
            last = volume->ext4.group_count;
        for (uint64_t g = first_member; g < last; ++g) {
            if (g == goal_group)
                continue;
            ret = ext4_scan_group(volume, g, 0, count, false, window_owner,
                                  &best);
            if (ret < 0)
                return ret;
            if (ret == 1)
                goto commit;
        }
    }

    /* Phases 3/4: other flex groups, candidates whose free count can
     * satisfy the request first, then the rest. */
    for (uint32_t tier = 0; tier < 2; ++tier) {
        for (uint64_t flex = 0; flex < flex_count; ++flex) {
            uint64_t flex_free;
            uint64_t first_member, last;

            if (flex == goal_flex)
                continue;
            ret = ext4_flex_free_blocks(volume, flex, &flex_free);
            if (ret < 0)
                return ret;
            if (tier == 0 ? flex_free < count : flex_free >= count)
                continue;
            if (flex_free == 0)
                continue;
            first_member = flex * (uint64_t)ext4_flex_size(volume);
            last = first_member + ext4_flex_size(volume);
            if (last > volume->ext4.group_count)
                last = volume->ext4.group_count;
            for (uint64_t g = first_member; g < last; ++g) {
                ret = ext4_scan_group(volume, g, 0, count, false,
                                      window_owner, &best);
                if (ret < 0)
                    return ret;
                if (ret == 1)
                    goto commit;
            }
        }
    }

    /* No full run anywhere: the best shorter run, or ENOSPC. */
    if (best.len == 0)
        return -RELIEFOS_ENOSPC;

commit:
    ret = ext4_commit_run(volume, best.first, best.len, true);
    if (ret < 0)
        return ret;
    *first = best.first;
    *allocated = best.len;
    volume->ext4.next_goal_block = best.first + best.len;
    if (volume->ext4.next_goal_block >= volume->ext4.blocks_count)
        volume->ext4.next_goal_block = volume->ext4.first_data_block;
    volume->ext4.reserved_window_end =
        volume->ext4.next_goal_block + EXT4_RESERVATION_WINDOW_BLOCKS;
    if (volume->ext4.reserved_window_end > volume->ext4.blocks_count)
        volume->ext4.reserved_window_end = volume->ext4.blocks_count;
    return 0;
}

static int ext4_free_blocks_impl(struct storage_volume *volume, uint64_t first,
                             uint32_t count)
{
    uint64_t end;

    if (!ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    if (count == 0 || first < volume->ext4.first_data_block)
        return -RELIEFOS_EINVAL;
    if (count > volume->ext4.blocks_count - first)
        return -RELIEFOS_EINVAL;
    end = first + count;

    /* Validate everything before mutating anything (all-or-nothing). */
    {
        uint64_t block = first;
        while (block < end) {
            struct storage_ext4_group_view view;
            uint64_t group = ext4_group_of_block(volume, block);
            /* Block numbers are 64-bit: never narrow the group end, a
             * truncated boundary below `block` would not advance the
             * inner loop and hang the outer one. */
            uint64_t group_end = ext4_group_first_block(volume, group) +
                                 ext4_group_block_count(volume, group);
            uint8_t *bitmap;
            int ret;

            ret = ext4_group_summary(volume, group, &view);
            if (ret < 0)
                return ret;
            if ((view.flags & EXT4_BG_BLOCK_UNINIT) != 0)
                return -RELIEFOS_EINVAL; /* all free: this is a double free */
            ret = storage_ext4_cache_get(volume, view.block_bitmap, &bitmap,
                                         false);
            if (ret < 0)
                return ret;
            ret = ext4_bitmap_csum_verify(volume, bitmap,
                                          volume->ext4.blocks_per_group / 8u,
                                          view.block_bitmap_csum, 0);
            if (ret < 0) {
                ext4_mark_volume_error(volume, group, 0);
                return ret;
            }
            while (block < end && block < group_end) {
                uint32_t bit =
                    (uint32_t)(block - ext4_group_first_block(volume, group));
                uint64_t base =
                    ext4_group_first_block(volume, group) +
                    ext4_base_meta_blocks(volume, group);

                if (!ext4_test_bit(bitmap, bit))
                    return -RELIEFOS_EINVAL; /* double free */
                if (block < base || block == view.block_bitmap ||
                    block == view.inode_bitmap ||
                    (block >= view.inode_table &&
                     block < view.inode_table + ext4_group_itb(volume)))
                    return -RELIEFOS_EINVAL; /* metadata is not data */
                ++block;
            }
        }
    }

    /* Commit (rollback on failure: the inverse operation restores the
     * already-cleared chunks). */
    {
        int ret = ext4_commit_run(volume, first, count, false);
        if (ret < 0)
            return ret;
    }
    volume->ext4.reserved_window_end = 0; /* recycling makes it stale */
    return 0;
}

/* ---- inode allocation -------------------------------------------------- */
static int ext4_inode_bit_range(const struct storage_volume *volume,
                                uint64_t group, uint32_t *lo, uint32_t *hi)
{
    uint64_t group_ino = group * (uint64_t)volume->ext4.inodes_per_group;
    uint64_t rest;

    if (group_ino >= volume->ext4.inodes_count)
        return -RELIEFOS_EINVAL;
    rest = volume->ext4.inodes_count - group_ino;
    *hi = (uint32_t)(rest < volume->ext4.inodes_per_group
                         ? rest
                         : volume->ext4.inodes_per_group);
    /* Inodes 1..EXT4_GOOD_OLD_FIRST_INO-1 are reserved forever. */
    *lo = 0;
    if (group == 0)
        *lo = EXT4_GOOD_OLD_FIRST_INO - 1u;
    return 0;
}

static int ext4_alloc_inode_in_group(struct storage_volume *volume, uint64_t group,
                                     bool directory, uint64_t *ino)
{
    struct storage_ext4_group_view view;
    uint8_t *bitmap;
    uint32_t lo, hi;
    uint32_t found = UINT32_MAX;
    int ret;

    ret = ext4_inode_bit_range(volume, group, &lo, &hi);
    if (ret < 0)
        return ret;
    /* The search runs unpinned: the -ENOSPC and guard paths below must
     * not leave a write pin behind (the outer per-group retry would
     * otherwise stack one pin per group and pin the cache shut). */
    ret = ext4_load_inode_bitmap(volume, group, &view, &bitmap, false);
    if (ret < 0)
        return ret;
    for (uint32_t bit = lo; bit < hi; ++bit) {
        if (!ext4_test_bit(bitmap, bit)) {
            found = bit;
            break;
        }
    }
    if (found == UINT32_MAX)
        return -RELIEFOS_ENOSPC;
    if (view.free_inodes_count == 0) {
        volume->ext4.fs_error = 1;
        return -RELIEFOS_EIO;
    }

    /* Pin the bitmap only now, for the mutation itself. */
    ret = storage_ext4_cache_get(volume, view.inode_bitmap, &bitmap, true);
    if (ret < 0)
        return ret;
    ext4_set_bit(bitmap, found);
    view.free_inodes_count--;
    if (directory)
        view.used_dirs_count++;
    /* Linux ext4_mark_inode_used(): the unused inode-table tail shrinks
     * below the highest inode ever taken from this group. */
    {
        uint32_t used_prefix =
            volume->ext4.inodes_per_group - view.itable_unused;
        if (found >= used_prefix && volume->ext4.inodes_per_group > found)
            view.itable_unused = volume->ext4.inodes_per_group - found - 1u;
    }
    ext4_bitmap_csum_set(volume, bitmap, volume->ext4.inodes_per_group / 8u,
                         &view, 1);
    ret = storage_ext4_write_group(volume, group, &view);
    if (ret < 0) {
        (void)storage_ext4_cache_mark_dirty(volume, view.inode_bitmap);
        return ret;
    }
    /* journal handle: task 7 */
    ret = storage_ext4_cache_mark_dirty(volume, view.inode_bitmap);
    if (ret < 0)
        return ret;
    ret = ext4_super_counts_update(volume, 0, -1);
    if (ret < 0)
        return ret;
    *ino = group * (uint64_t)volume->ext4.inodes_per_group + found + 1u;
    return 0;
}

static int ext4_alloc_inode_impl(struct storage_volume *volume, bool directory,
                             uint64_t *ino)
{
    uint64_t preferred = 0;
    bool have_preferred = false;

    if (!ino || !ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    *ino = 0;

    if (directory) {
        uint32_t best_free = 0;
        for (uint64_t g = 0; g < volume->ext4.group_count; ++g) {
            struct storage_ext4_group_view view;
            int ret = ext4_group_summary(volume, g, &view);

            if (ret < 0)
                return ret;
            if (view.free_inodes_count > best_free) {
                best_free = view.free_inodes_count;
                preferred = g;
                have_preferred = true;
            }
        }
        if (!have_preferred)
            return -RELIEFOS_ENOSPC;
    } else {
        for (uint64_t g = 0; g < volume->ext4.group_count; ++g) {
            struct storage_ext4_group_view view;
            int ret = ext4_group_summary(volume, g, &view);

            if (ret < 0)
                return ret;
            if (view.free_inodes_count > 0) {
                preferred = g;
                have_preferred = true;
                break;
            }
        }
        if (!have_preferred)
            return -RELIEFOS_ENOSPC;
    }

    /* Preferred group first, then every other group (a stale free count
     * must not turn into ENOSPC while bits remain). */
    {
        int ret = ext4_alloc_inode_in_group(volume, preferred, directory, ino);
        if (ret != -RELIEFOS_ENOSPC)
            return ret;
        for (uint64_t g = 0; g < volume->ext4.group_count; ++g) {
            if (g == preferred)
                continue;
            ret = ext4_alloc_inode_in_group(volume, g, directory, ino);
            if (ret != -RELIEFOS_ENOSPC)
                return ret;
        }
    }
    return -RELIEFOS_ENOSPC;
}

static int ext4_free_inode_impl(struct storage_volume *volume, uint64_t ino,
                            bool directory)
{
    struct storage_ext4_group_view view;
    uint8_t *bitmap;
    uint64_t group;
    uint32_t bit;
    uint64_t table_block, table_off;
    uint8_t *slot;
    int ret;

    if (!ext4_geometry_ok(volume))
        return -RELIEFOS_EINVAL;
    if (ino < EXT4_GOOD_OLD_FIRST_INO || ino > volume->ext4.inodes_count)
        return -RELIEFOS_EINVAL;
    group = (ino - 1) / volume->ext4.inodes_per_group;
    bit = (uint32_t)((ino - 1) % volume->ext4.inodes_per_group);

    ret = ext4_group_summary(volume, group, &view);
    if (ret < 0)
        return ret;
    if ((view.flags & EXT4_BG_INODE_UNINIT) != 0)
        return -RELIEFOS_EINVAL; /* all free: this is a double free */

    /* All validation runs read-only so the -EINVAL paths never hold a
     * write pin (the bounded cache would leak it as an unpinnable slot). */
    ret = storage_ext4_cache_get(volume, view.inode_bitmap, &bitmap, false);
    if (ret < 0)
        return ret;
    ret = ext4_bitmap_csum_verify(volume, bitmap,
                                  volume->ext4.inodes_per_group / 8u,
                                  view.inode_bitmap_csum, 1);
    if (ret < 0) {
        ext4_mark_volume_error(volume, group, 1);
        return ret;
    }
    if (!ext4_test_bit(bitmap, bit))
        return -RELIEFOS_EINVAL; /* double free */

    /* The caller must have dropped the last link. */
    table_block = view.inode_table +
                  ((uint64_t)bit * volume->ext4.inode_size) /
                      volume->ext4.block_size;
    table_off = (((uint64_t)bit * volume->ext4.inode_size) %
                 volume->ext4.block_size);
    ret = storage_ext4_cache_read(volume, table_block, ext4_alloc_block);
    if (ret < 0)
        return ret;
    if (ext4_get_le16(ext4_alloc_block + table_off + AGD_INO_LINKS) != 0)
        return -RELIEFOS_EINVAL;

    /* Zero the inode-table slot through the cache. */
    ret = storage_ext4_cache_get(volume, table_block, &slot, true);
    if (ret < 0)
        return ret;
    storage_memzero(slot + table_off, volume->ext4.inode_size);
    /* journal handle: task 7 */
    ret = storage_ext4_cache_mark_dirty(volume, table_block);
    if (ret < 0)
        return ret;

    ret = storage_ext4_cache_get(volume, view.inode_bitmap, &bitmap, true);
    if (ret < 0)
        return ret;
    bitmap[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
    view.free_inodes_count++;
    if (directory && view.used_dirs_count > 0)
        view.used_dirs_count--;
    ext4_bitmap_csum_set(volume, bitmap, volume->ext4.inodes_per_group / 8u,
                         &view, 1);
    ret = storage_ext4_write_group(volume, group, &view);
    if (ret < 0) {
        (void)storage_ext4_cache_mark_dirty(volume, view.inode_bitmap);
        return ret;
    }
    ret = storage_ext4_cache_mark_dirty(volume, view.inode_bitmap);
    if (ret < 0)
        return ret;
    return ext4_super_counts_update(volume, 0, 1);
}

/* Nested allocator calls join an outer inode/directory transaction. Standalone
 * calls own a transaction; errors restore all captured metadata and summaries. */
static int ext4_allocator_stop(struct storage_ext4_handle *handle, int result)
{
    if (result < 0) storage_ext4_journal_abort(handle, result);
    int stop = storage_ext4_journal_stop(handle);
    return result < 0 ? result : stop;
}
int storage_ext4_alloc_blocks(struct storage_volume *v, uint64_t goal, uint32_t count,
                              uint64_t *first, uint32_t *allocated)
{
    if (first) *first = 0;
    if (allocated) *allocated = 0;
    struct storage_ext4_handle h;
    int ret = storage_ext4_journal_start(v, EXT4_JOURNAL_CREDITS, &h);
    if (ret < 0) return ret;
    ret = ext4_allocator_stop(&h, ext4_alloc_blocks_impl(v, goal, count, first, allocated));
    if (ret < 0) { if (first) *first = 0; if (allocated) *allocated = 0; }
    return ret;
}
int storage_ext4_free_blocks(struct storage_volume *v, uint64_t first, uint32_t count)
{
    struct storage_ext4_handle h;
    int ret = storage_ext4_journal_start(v, EXT4_JOURNAL_CREDITS, &h);
    if (ret < 0) return ret;
    return ext4_allocator_stop(&h, ext4_free_blocks_impl(v, first, count));
}
int storage_ext4_alloc_inode(struct storage_volume *v, bool directory, uint64_t *ino)
{
    if (ino) *ino = 0;
    struct storage_ext4_handle h;
    int ret = storage_ext4_journal_start(v, EXT4_JOURNAL_CREDITS, &h);
    if (ret < 0) return ret;
    ret = ext4_allocator_stop(&h, ext4_alloc_inode_impl(v, directory, ino));
    if (ret < 0 && ino) *ino = 0;
    return ret;
}
int storage_ext4_free_inode(struct storage_volume *v, uint64_t ino, bool directory)
{
    struct storage_ext4_handle h;
    int ret = storage_ext4_journal_start(v, EXT4_JOURNAL_CREDITS, &h);
    if (ret < 0) return ret;
    return ext4_allocator_stop(&h, ext4_free_inode_impl(v, ino, directory));
}

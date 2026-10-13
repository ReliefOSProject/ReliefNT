/* ext-family mount: superblock/group-descriptor reads through bounded block
 * I/O, the feature policy of record, ext2-vs-ext4 classification and
 * per-volume ext4 state.  Format parsing and checksum primitives come from
 * storage_ext4_format.c / storage_ext4_checksum.c; the bounded caches from
 * storage_ext4_cache.c.  See storage_internal.h for the contract (verdicts,
 * read_only_reason composition and failure cleanup). */

/* ext4_inode field offsets (linux/fs/ext4/ext4.h:794). */
#define INO_MODE 0x00
#define INO_GENERATION 0x64

/* Feature masks the policy of record allows or rejects (ruling 1). */
#define EXT4_INCOMPAT_ALLOWED                                                  \
    (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER |          \
     EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |             \
     EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED)
#define EXT4_RO_COMPAT_RW                                                      \
    (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE | \
     EXT4_FEATURE_RO_COMPAT_BTREE_DIR | EXT4_FEATURE_RO_COMPAT_HUGE_FILE |     \
     EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_DIR_NLINK |      \
     EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
#define EXT4_RO_COMPAT_REJECT                                                  \
    (EXT4_FEATURE_RO_COMPAT_QUOTA | EXT4_FEATURE_RO_COMPAT_BIGALLOC |          \
     EXT4_FEATURE_RO_COMPAT_PROJECT | EXT4_FEATURE_RO_COMPAT_VERITY)

/* Assembled root inode for the straddling case (kernel stacks stay small). */
static uint8_t ext4_mount_inode_buf[EXT4_MAX_BLOCK_SIZE];

/* Bounded raw block read over the volume's ext4 geometry. */
static int ext4_mount_read_block(struct storage_volume *volume, uint64_t block, void *buf)
{
    uint64_t lba;
    uint32_t sectors;
    int ret = ext4_cache_block_range(volume, block, &lba, &sectors);
    if (ret < 0) {
        return ret;
    }
    return storage_read_device(volume, lba, sectors, buf);
}

/* Feature policy of record (ruling 1/2): returns 0 with verdict RW/RO plus a
 * read_only_reason, or -RELIEFOS_EOPNOTSUPP with verdict REJECT after
 * logging the offending bit mask. */
int storage_ext4_feature_policy(const struct storage_ext4_super_view *view,
                                struct storage_ext4_feature_decision *out)
{
    uint32_t bad;
    uint32_t reason = STORAGE_EXT4_READ_ONLY_NONE;

    if (!view || !out) {
        return -RELIEFOS_EINVAL;
    }
    out->verdict = STORAGE_EXT4_FEATURE_RW;
    out->read_only_reason = STORAGE_EXT4_READ_ONLY_NONE;

    /* Incompat: only the recognized set mounts at all; the explicit reject
     * list and any unknown bit fail closed (unknown bits change on-disk
     * interpretation). */
    bad = view->feature_incompat & ~EXT4_INCOMPAT_ALLOWED;
    if (bad != 0) {
        console_printf("[reliefnt] ext4 mount rejected incompat=0x%x\n", bad);
        out->verdict = STORAGE_EXT4_FEATURE_REJECT;
        return -RELIEFOS_EOPNOTSUPP;
    }
    /* Compat never blocks, except FAST_COMMIT: it changes how the journal is
     * interpreted (spec: affects on-disk interpretation -> reject). */
    if ((view->feature_compat & EXT4_FEATURE_COMPAT_FAST_COMMIT) != 0) {
        console_printf("[reliefnt] ext4 mount rejected compat=0x%x\n",
                       EXT4_FEATURE_COMPAT_FAST_COMMIT);
        out->verdict = STORAGE_EXT4_FEATURE_REJECT;
        return -RELIEFOS_EOPNOTSUPP;
    }
    /* Quota/bigalloc/project/verity change on-disk interpretation too. */
    bad = view->feature_ro_compat & EXT4_RO_COMPAT_REJECT;
    if (bad != 0) {
        console_printf("[reliefnt] ext4 mount rejected ro_compat=0x%x\n", bad);
        out->verdict = STORAGE_EXT4_FEATURE_REJECT;
        return -RELIEFOS_EOPNOTSUPP;
    }
    /* Read-only outcomes, composed with the precedence documented in the
     * header: permanent reasons first, so clearing RECOVER later can only
     * restore write access when it was the sole reason. */
    if ((view->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_READONLY) != 0) {
        reason = STORAGE_EXT4_READ_ONLY_READONLY_FEATURE;
    } else if ((view->feature_ro_compat &
                ~(EXT4_RO_COMPAT_RW | EXT4_RO_COMPAT_REJECT |
                  EXT4_FEATURE_RO_COMPAT_READONLY)) != 0) {
        reason = STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT;
    } else if ((view->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) != 0) {
        /* Mount clears this provisional reason only after successful replay.
         * A bare RECOVER without HAS_JOURNAL cannot be recovered safely. */
        reason = STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY;
    }
    if (reason != STORAGE_EXT4_READ_ONLY_NONE) {
        out->verdict = STORAGE_EXT4_FEATURE_RO;
        out->read_only_reason = reason;
    }
    return 0;
}

/* Ruling 3: any ext3/4-era feature classifies as EXT4; only a feature set
 * made of classic ext2 bits is "pure ext2".  Anything else stays with the
 * new backend so its read-only policy is enforced. */
static uint8_t ext4_classify(const struct storage_ext4_super_view *view)
{
    if ((view->feature_compat &
         (EXT4_FEATURE_COMPAT_HAS_JOURNAL | EXT4_FEATURE_COMPAT_SPARSE_SUPER2)) != 0) {
        return STORAGE_FILESYSTEM_EXT4;
    }
    if ((view->feature_incompat & (EXT4_FEATURE_INCOMPAT_EXTENTS |
                                   EXT4_FEATURE_INCOMPAT_64BIT |
                                   EXT4_FEATURE_INCOMPAT_FLEX_BG)) != 0) {
        return STORAGE_FILESYSTEM_EXT4;
    }
    if ((view->feature_ro_compat & (EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |
                                    EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                                    EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
                                    EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
                                    EXT4_FEATURE_RO_COMPAT_GDT_CSUM)) != 0) {
        return STORAGE_FILESYSTEM_EXT4;
    }
    if ((view->feature_compat &
         ~(EXT4_FEATURE_COMPAT_DIR_PREALLOC | EXT4_FEATURE_COMPAT_IMAGIC_INODES |
           EXT4_FEATURE_COMPAT_EXT_ATTR | EXT4_FEATURE_COMPAT_RESIZE_INODE |
           EXT4_FEATURE_COMPAT_DIR_INDEX)) == 0 &&
        (view->feature_incompat & ~EXT4_FEATURE_INCOMPAT_FILETYPE) == 0 &&
        (view->feature_ro_compat &
         ~(EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
           EXT4_FEATURE_RO_COMPAT_BTREE_DIR)) == 0) {
        return STORAGE_FILESYSTEM_EXT2;
    }
    return STORAGE_FILESYSTEM_EXT4;
}

void storage_ext4_state_reset(struct storage_volume *volume)
{
    if (!volume) {
        return;
    }
    storage_ext4_journal_close(volume);
    storage_ext4_cache_invalidate(volume);
    storage_memzero(&volume->ext4, sizeof(volume->ext4));
    volume->read_only_reason = STORAGE_EXT4_READ_ONLY_NONE;
    volume->filesystem = STORAGE_FILESYSTEM_NONE;
}

int storage_ext4_mount(struct storage_volume *volume)
{
    struct storage_ext4_super_view view;
    struct storage_ext4_feature_decision decision;
    struct storage_ext4_group_view group_view;
    uint64_t gdt_block;
    uint64_t ino_table = 0;
    uint64_t ino_offset;
    uint64_t ino_block;
    uint32_t ino_in_block;
    uint32_t creator_os;
    uint32_t orphan_head;
    uint32_t filesystem;
    uint8_t flex_log;
    const uint8_t *ino_raw;
    const char *reason;
    int ret;

    if (!volume) {
        return -RELIEFOS_EINVAL;
    }
    if (volume->ext4.block_size && !volume->ext4.journal) {
        ret = storage_ext4_cache_flush(volume);
        if (ret < 0) return ret;
    }
    if (volume->ext4.journal) {
        ret = storage_ext4_journal_commit(volume, true);
        if (!ret) ret = storage_ext4_journal_checkpoint(volume);
        if (ret < 0) return ret;
        storage_ext4_journal_close(volume);
    }
    storage_memzero(&volume->ext4, sizeof(volume->ext4));
    volume->read_only_reason = STORAGE_EXT4_READ_ONLY_NONE;
    /* Every mount attempt gets a fresh generation so cache entries from a
     * previous mount of this volume object are never served. */
    volume->mount_generation++;
    /* Drop every cached mapping for this volume before the first cached read,
     * not after the group-descriptor pass.  Both the physical root and the
     * installer ramdisk live in g_volumes[STORAGE_VOLUME_ROOT], and the RAM-root
     * path memzeros that object, which resets mount_generation to zero before
     * this increment -- so the disk's entries are stamped with the same
     * generation the ramdisk will use and every cache guard matches them.
     * Two of those caches matter early: the block cache and the group-descriptor
     * summary.  A stale summary hands the ramdisk the *disk's* inode_table
     * block, so reading journal inode 8 fetches unrelated data, the inode
     * checksum fails, and the mount falls back to read-only because the journal
     * looks corrupt -- which then stops userland on /run (a black screen). */
    storage_ext4_cache_invalidate(volume);
    storage_ext4_invalidate_groups(volume);
    if ((!volume->ext_start_lba && volume->kind != STORAGE_VOLUME_RAM) ||
        volume->ext_sector_count < 4u) {
        ret = -RELIEFOS_EINVAL;
        goto fail;
    }

    /* The superblock lives at byte offset 1024 of the partition: sectors
     * 2-3, both 512-byte aligned. */
    ret = storage_read_device(volume, volume->ext_start_lba + 2u, 2u, storage_scratch);
    if (ret < 0) {
        goto fail;
    }
    ret = storage_ext4_parse_super(storage_scratch, EXT4_SUPERBLOCK_SIZE, &view);
    if (ret < 0) {
        goto fail;
    }
    creator_os = ext4_get_le32(storage_scratch + SB_CREATOR_OS);
    orphan_head = ext4_get_le32(storage_scratch + 0xe8);
    if ((view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0) {
        ret = storage_ext4_verify_super_checksum(storage_scratch, EXT4_SUPERBLOCK_SIZE);
        if (ret < 0) {
            goto fail;
        }
    }
    /* The filesystem must fit the partition that claims to hold it. */
    if (view.blocks_count > volume->ext_sector_count / (view.block_size / SECTOR_SIZE)) {
        ret = -RELIEFOS_EINVAL;
        goto fail;
    }
    ret = storage_ext4_feature_policy(&view, &decision);
    if (ret < 0) {
        goto fail;
    }
    filesystem = ext4_classify(&view);

    /* Publish the geometry before the cache-backed reads below. */
    volume->ext4.partition_start_lba = volume->ext_start_lba;
    volume->ext4.partition_sector_count = volume->ext_sector_count;
    volume->ext4.blocks_count = view.blocks_count;
    volume->ext4.group_count = view.group_count;
    volume->ext4.block_size = view.block_size;
    volume->ext4.cluster_size = view.cluster_size;
    volume->ext4.readahead_blocks = 0;
    volume->ext4.blocks_per_group = view.blocks_per_group;
    volume->ext4.inodes_per_group = view.inodes_per_group;
    volume->ext4.inodes_count = view.inodes_count;
    volume->ext4.inode_size = view.inode_size;
    volume->ext4.first_data_block = view.first_data_block;
    volume->ext4.desc_size = view.desc_size;
    /* Transitional raw-block/stat wrappers use these geometry aliases only. */
    volume->ext2_block_size = view.block_size;
    volume->ext2_blocks_count = view.blocks_count > UINT32_MAX ? UINT32_MAX : view.blocks_count;
    volume->ext2_blocks_per_group = view.blocks_per_group;
    volume->ext2_inodes_per_group = view.inodes_per_group;
    volume->ext2_inode_size = view.inode_size;
    volume->ext2_first_data_block = view.first_data_block;
    volume->ext2_group_count = view.group_count;
    volume->ext2_feature_incompat = view.feature_incompat;
    volume->ext2_next_block = view.first_data_block;
    volume->ext2_next_inode = 11;

    /* Allocator context (task 5): the parsed superblock feeds the metadata
     * checksums and the free-count bookkeeping; the raw words
     * storage_ext4_parse_super() does not carry are read here (shared
     * SB_* spellings).  s_log_groups_per_flex is clamped to
     * EXT4_MAX_FLEX_LOG so the per-flex free-count sums stay bounded. */
    volume->ext4.super_view = view;
    volume->ext4.reserved_gdt_blocks =
        ext4_get_le16(storage_scratch + SB_RESERVED_GDT);
    volume->ext4.backup_bgs[0] = ext4_get_le32(storage_scratch + SB_BACKUP_BGS);
    volume->ext4.backup_bgs[1] =
        ext4_get_le32(storage_scratch + SB_BACKUP_BGS + 4);
    flex_log = storage_scratch[SB_LOG_GROUPS_PER_FLEX];
    if (flex_log > EXT4_MAX_FLEX_LOG) {
        console_printf("[reliefnt] ext4 mount clamped s_log_groups_per_flex "
                       "%u -> %u\n",
                       (unsigned)flex_log, (unsigned)EXT4_MAX_FLEX_LOG);
        flex_log = (uint8_t)EXT4_MAX_FLEX_LOG;
    }
    volume->ext4.flex_log_groups = flex_log;

    /* Recovery must precede the root inode and full descriptor validation.
     * Permanent RO policies prohibit recovery writes as well. */
    volume->read_only_reason = decision.read_only_reason;
    if (view.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        ret = storage_ext4_journal_open(volume);
        if (ret == -RELIEFOS_EOPNOTSUPP || ret == -RELIEFOS_ENOMEM) goto fail;
        if (!ret && (decision.read_only_reason == STORAGE_EXT4_READ_ONLY_NONE ||
                     decision.read_only_reason == STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY)) {
            ret = storage_ext4_journal_replay(volume);
            if (!ret) {
                ret = storage_read_device(volume, volume->ext_start_lba + 2u, 2u, storage_scratch);
                if (ret < 0) goto fail;
                ret = storage_ext4_parse_super(storage_scratch, EXT4_SUPERBLOCK_SIZE, &view);
                if (ret < 0) goto fail;
                if (view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
                    ret = storage_ext4_verify_super_checksum(storage_scratch, EXT4_SUPERBLOCK_SIZE);
                    if (ret < 0) goto fail;
                }
                if (view.block_size != volume->ext4.block_size ||
                    view.blocks_count != volume->ext4.blocks_count ||
                    view.desc_size != volume->ext4.desc_size ||
                    view.inode_size != volume->ext4.inode_size ||
                    view.inodes_per_group != volume->ext4.inodes_per_group ||
                    view.blocks_per_group != volume->ext4.blocks_per_group) {
                    ret = -RELIEFOS_EIO; goto fail;
                }
                volume->ext4.super_view = view;
                orphan_head = ext4_get_le32(storage_scratch + 0xe8);
                ret = storage_ext4_feature_policy(&view, &decision);
                if (ret < 0) goto fail;
            }
        }
        if (ret < 0) decision.read_only_reason = STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT;
    }

    /* Group descriptors start at block s_first_data_block + 1 and are
     * verified group by group. */
    gdt_block = (uint64_t)view.first_data_block + 1u;
    if (view.group_count > UINT64_MAX / view.desc_size) {
        ret = -RELIEFOS_EINVAL;
        goto fail;
    }
    for (uint64_t group = 0; group < view.group_count; ++group) {
        uint64_t offset = group * view.desc_size;
        uint64_t block = gdt_block + offset / view.block_size;
        uint32_t in_block = (uint32_t)(offset % view.block_size);
        uint8_t desc[EXT4_MAX_DESC_SIZE];

        if (in_block + view.desc_size <= view.block_size) {
            ret = ext4_mount_read_block(volume, block, storage_scratch);
            if (ret < 0) {
                goto fail;
            }
            storage_memcpy(desc, storage_scratch + in_block, view.desc_size);
        } else {
            uint32_t first = view.block_size - in_block;
            ret = ext4_mount_read_block(volume, block, storage_scratch);
            if (ret < 0) {
                goto fail;
            }
            storage_memcpy(desc, storage_scratch + in_block, first);
            ret = ext4_mount_read_block(volume, block + 1u, storage_scratch);
            if (ret < 0) {
                goto fail;
            }
            storage_memcpy(desc + first, storage_scratch, view.desc_size - first);
        }
        ret = storage_ext4_parse_group_desc(desc, view.desc_size, view.desc_size, &group_view);
        if (ret < 0) {
            goto fail;
        }
        ret = storage_ext4_verify_group_checksum(desc, view.desc_size, (uint32_t)group, &view);
        if (ret < 0) {
            goto fail;
        }
        if (group == 0) {
            ino_table = group_view.inode_table;
        }
    }

    /* Fresh cache generation for this mount, then the root inode through
     * the bounded block cache. */
    storage_ext4_cache_invalidate(volume);
    ino_offset = (uint64_t)(EXT2_ROOT_INO - 1u) * view.inode_size;
    ino_block = ino_table + ino_offset / view.block_size;
    ino_in_block = (uint32_t)(ino_offset % view.block_size);
    ret = storage_ext4_cache_read(volume, ino_block, storage_scratch);
    if (ret < 0) {
        goto fail;
    }
    if (ino_in_block + view.inode_size <= view.block_size) {
        ino_raw = storage_scratch + ino_in_block;
    } else {
        uint32_t first = view.block_size - ino_in_block;
        storage_memcpy(ext4_mount_inode_buf, storage_scratch + ino_in_block, first);
        ret = storage_ext4_cache_read(volume, ino_block + 1u, storage_scratch);
        if (ret < 0) {
            goto fail;
        }
        storage_memcpy(ext4_mount_inode_buf + first, storage_scratch, view.inode_size - first);
        ino_raw = ext4_mount_inode_buf;
    }
    if ((view.feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0 &&
        creator_os == 0) {
        /* inode checksums are only defined for Linux-created filesystems
         * (storage_ext4_checksum.c contract). */
        ret = storage_ext4_verify_inode_checksum(ino_raw, view.inode_size, EXT2_ROOT_INO,
                                                 ext4_get_le32(ino_raw + INO_GENERATION),
                                                 &view);
        if (ret < 0) {
            goto fail;
        }
        ext4_cache_note_checksum_ok(volume, ino_block);
    }
    if ((ext4_get_le16(ino_raw + INO_MODE) & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        ret = -RELIEFOS_EIO;
        goto fail;
    }

    volume->filesystem = (uint8_t)filesystem;
    volume->read_only_reason = decision.read_only_reason;
    if (!volume->read_only_reason && orphan_head) {
        ret = storage_ext4_recover_orphans(volume);
        if (ret < 0) goto fail;
    }
    reason = decision.read_only_reason == STORAGE_EXT4_READ_ONLY_READONLY_FEATURE
                 ? " readonly-feature"
                 : decision.read_only_reason == STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT
                       ? " unknown-ro-compat"
                       : decision.read_only_reason ==
                                     STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY
                             ? " journal-needs-recovery"
                             : decision.read_only_reason == STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT
                                   ? " journal-corrupt" : "";
    console_printf("[reliefnt] ext4 mount ok fs=%s blocks=%llu groups=%llu%s\n",
                   filesystem == STORAGE_FILESYSTEM_EXT4 ? "ext4" : "ext2",
                   (unsigned long long)view.blocks_count,
                   (unsigned long long)view.group_count, reason);
    return 0;

fail:
    storage_ext4_state_reset(volume);
    return ret;
}

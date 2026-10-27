/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXT4_INTERNAL_H
#define RELIEFNT_EXT4_INTERNAL_H

#include <linux/mount.h>
#include <reliefos/fs_abi.h>
#include <reliefnt/storage.h>

#include "storage_ext4_types.h"

/**
 * @file internal.h
 * @brief Private ext4 subsystem interfaces shared by ext4 translation units.
 */

struct storage_volume;

int storage_ext4_journal_open(struct storage_volume *);
/**
 * @brief Admit a non-replayable metadata transaction and finish prior journal work.
 * @param v Borrowed live ext-family instance under storage execution ownership.
 * @param credits Positive maximum block-image count, bounded by journal capacity.
 * @param h Required caller-owned handle, empty on validation/admission failure.
 * @return Zero with an active handle requiring stop, or negative validation,
 * readonly, recovery, commit/checkpoint or I/O error. No module/graph lock may
 * span this call. Admission latches storage mutation before any old-journal
 * read or write: a resumable read must not be mistaken for journal corruption.
 * The latch stays set on failure; native metadata is not replayed after yield.
 */
int storage_ext4_journal_start(struct storage_volume *v, uint32_t credits, struct storage_ext4_handle *h);
int storage_ext4_journal_dirty(struct storage_ext4_handle *, uint64_t);
int storage_ext4_journal_stop(struct storage_ext4_handle *);
void storage_ext4_journal_abort(struct storage_ext4_handle *, int);
int storage_ext4_journal_commit(struct storage_volume *, bool);
int storage_ext4_journal_checkpoint(struct storage_volume *);
int storage_ext4_journal_replay(struct storage_volume *);
void storage_ext4_journal_close(struct storage_volume *);
/* Cache integration: capture before exposing writable bytes, publish after
 * modification, retain ownership until checkpoint or rollback. */
int storage_ext4_journal_capture(struct storage_volume *, uint64_t, const uint8_t *);
int storage_ext4_journal_publish(struct storage_volume *, uint64_t, const uint8_t *);
bool storage_ext4_journal_owns(const struct storage_volume *, uint64_t);
void storage_ext4_cache_finish(struct storage_volume *, uint64_t, const uint8_t *);
/** @brief Evict clean sector-overlapping cache aliases of the same block identity.
 * @param writer Borrowed pinned instance. @param lba First sector. @param count Sector count.
 * Storage execution exclusion required; dirty/pinned journal data is preserved.
 */
void storage_ext4_cache_invalidate_write(const struct storage_volume *writer,uint64_t lba,uint32_t count);
/** @brief Flush ext-family backing through the registered provider even during mount.
 * @param volume Borrowed instance with a live device reference; ready may be false.
 * @return Zero or negative transport errno. Takes transport exclusion only.
 */
int storage_ext4_device_flush(const struct storage_volume *volume);
int storage_ext4_parse_inode(const uint8_t *, uint32_t,
                             const struct storage_ext4_super_view *, struct ext4_inode_view *);
int storage_ext4_read_inode(struct storage_volume *, uint64_t, struct ext4_inode_view *);
int storage_ext4_write_inode(struct storage_volume *, uint64_t, const struct ext4_inode_view *);
int storage_ext4_write_file_range(struct storage_volume *, uint64_t, uint64_t,
                                  const void *, uint32_t, uint32_t *);
int storage_ext4_truncate(struct storage_volume *, uint64_t, uint64_t);
int storage_ext4_destroy_inode(struct storage_volume *, uint64_t);
int storage_ext4_getxattr(struct storage_volume *,uint64_t,const char *,void *,uint32_t,uint32_t *);
int storage_ext4_setxattr(struct storage_volume *,uint64_t,const char *,const void *,uint32_t,uint32_t);
int storage_ext4_removexattr(struct storage_volume *,uint64_t,const char *);
int storage_ext4_drop_xattrs(struct storage_volume *,uint64_t,struct ext4_inode_view *);
int storage_ext4_recover_orphans(struct storage_volume *);
int storage_ext4_fallocate(struct storage_volume *, uint64_t, uint32_t, uint64_t, uint64_t);
int storage_ext4_change_mapping(struct storage_volume *, uint64_t, struct ext4_inode_view *,
                                uint64_t, uint64_t, uint32_t, bool, bool);
int storage_ext4_map_block(struct storage_volume *, uint64_t, const struct ext4_inode_view *,
                           uint64_t, bool, struct storage_ext4_map_result *);
int storage_ext4_insert_extent(struct storage_volume *, uint64_t, uint64_t, uint64_t,
                               uint32_t, bool);
int storage_ext4_remove_range(struct storage_volume *, uint64_t, uint64_t, uint64_t);
/* FIEMAP start/len are bytes. capacity=0 counts mappings without storing them.
 * A full output array reports the stored count; LAST is set only if iteration
 * reached EOF, not merely because the output capacity was exhausted. */
int storage_ext4_fiemap(struct storage_volume *, uint64_t, uint64_t, uint64_t,
                        struct storage_ext4_fiemap_extent *, uint32_t, uint32_t *);
int storage_ext4_read_file_range(struct storage_volume *, uint64_t,
                                const struct ext4_inode_view *, uint64_t,
                                void *, uint32_t, uint32_t *);
int storage_ext4_cache_read_blocks(struct storage_volume *, uint64_t, uint32_t, void *);
int storage_ext4_dir_iterate(struct storage_volume *,uint64_t,uint64_t *,struct storage_ext4_dirent *);
int storage_ext4_dir_lookup(struct storage_volume *,uint64_t,const char *,uint64_t *,uint8_t *);
int storage_ext4_dir_insert(struct storage_volume *,uint64_t,const char *,uint64_t,uint8_t);
int storage_ext4_dir_remove(struct storage_volume *,uint64_t,const char *,uint64_t *);
int storage_ext4_create(struct storage_volume *,uint64_t,const char *,uint16_t,uint32_t,uint32_t,const char *,uint64_t *);
int storage_ext4_create_node(struct storage_volume *,uint64_t,const char *,uint16_t,uint32_t,uint32_t,const char *,struct storage_node *);
int storage_ext4_link(struct storage_volume *,uint64_t,const char *,uint64_t);
int storage_ext4_unlink(struct storage_volume *,uint64_t,const char *,bool);
int storage_ext4_rename(struct storage_volume *,uint64_t,const char *,uint64_t,const char *);
int storage_ext4_orphan_add(struct storage_volume *,uint64_t,struct ext4_inode_view *);
int storage_ext4_unlink_held(struct storage_volume *,uint64_t,const char *,bool,bool);
int storage_ext4_rename_held(struct storage_volume *,uint64_t,const char *,uint64_t,const char *,bool);
void storage_ext4_invalidate_groups(struct storage_volume *);
int storage_ext4_check_node(struct storage_volume *,const struct storage_node *,struct ext4_inode_view *);
int storage_ext4_replace(struct storage_volume *,const char *,const void *,uint32_t);
int storage_ext4_mark_special(struct storage_volume *,const char *,const struct storage_node *,uint32_t);
struct storage_ext4_operations {
    int (*lookup)(struct storage_volume *,const char *,struct storage_node *);
    int (*read)(struct storage_volume *,const struct storage_node *,uint64_t,void *,uint32_t,uint32_t *);
    int (*write)(struct storage_volume *,const struct storage_node *,uint64_t,const void *,uint32_t,uint32_t *);
    int (*readdir)(struct storage_volume *,const struct storage_node *,uint64_t *,struct reliefos_dir_entry *);
    int (*create)(struct storage_volume *,const char *,uint16_t,uint32_t,uint32_t,struct storage_node *);
    int (*mkdir)(struct storage_volume *,const char *);
    int (*link)(struct storage_volume *,const char *,const char *);
    int (*unlink)(struct storage_volume *,const char *);
    int (*rmdir)(struct storage_volume *,const char *);
    int (*rename)(struct storage_volume *,const char *,const char *);
    int (*truncate)(struct storage_volume *,const struct storage_node *,uint64_t);
    int (*symlink)(struct storage_volume *,const char *,const char *);
    int (*readlink)(struct storage_volume *,const struct storage_node *,char *,uint32_t,uint32_t *);
    int (*fsync)(struct storage_volume *);
    int (*fallocate)(struct storage_volume *,uint64_t,uint32_t,uint64_t,uint64_t);
    int (*fiemap)(struct storage_volume *,uint64_t,uint64_t,uint64_t,struct storage_ext4_fiemap_extent *,uint32_t,uint32_t *);
    int (*getxattr)(struct storage_volume *,uint64_t,const char *,void *,uint32_t,uint32_t *);
    int (*setxattr)(struct storage_volume *,uint64_t,const char *,const void *,uint32_t,uint32_t);
    int (*statfs)(struct storage_volume *,struct linux_statfs_abi *);
};
extern const struct storage_ext4_operations storage_ext4_ops;
/** @brief Commit/checkpoint native journal data before flushing its dirty cache.
 * @param volume Borrowed pinned ext-family instance with parsed geometry.
 * @return Zero or first journal/cache error. Execution ownership and synchronous
 * I/O required; precedes physical device flush without transport/module locks.
 */
int storage_ext4_sync(struct storage_volume *volume);

int storage_ext4_feature_policy(const struct storage_ext4_super_view *view,
                                struct storage_ext4_feature_decision *out);
int storage_ext4_mount(struct storage_volume *volume);
/* Drops the volume's ext4 cache entries and clears ext4 state,
 * read_only_reason and the filesystem kind (mount failure and ext2
 * fallback cleanup). */
void storage_ext4_state_reset(struct storage_volume *volume);

/* ---- ext4 bounded caches (fs/ext_common/cache.c) ---------------------------
 * Four fixed-capacity caches (block / inode / dir / journal eviction
 * domains; capacity macros may be overridden with -D in host tests).  Each
 * entry is stamped valid/dirty/pinned/block/age/checksum_ok/volume_generation
 * and holds one filesystem block; entries are lazily allocated with
 * mm_alloc_pages like ext2_cache.  Eviction is LRU by age and never touches
 * pinned entries; when the capacity is full and every entry is pinned the
 * insert paths return -RELIEFOS_ENOMEM.  I/O is bounded by the volume's
 * ext4 geometry and partition range and never runs under the transport
 * spinlock.  The cache does no format parsing: checksum_ok is set by the
 * caller (storage_ext4_mount stamps blocks whose checksums it verified) and
 * cleared when a block is modified.
 *
 * Pin lifecycle: storage_ext4_cache_get(..., for_write=true) pins the entry
 * until storage_ext4_cache_mark_dirty() publishes the change and releases
 * the pin, or until flush/invalidate.  Reads never pin.
 * storage_ext4_cache_read() falls back to uncached device I/O when the table
 * cannot be allocated (ext2_cache convention: allocation failure only
 * disables caching for that I/O) but propagates -RELIEFOS_ENOMEM when the
 * capacity is full of pinned entries.
 *
 * Calling convention: a caller that remounts a volume (which increments
 * mount_generation and makes every cached block of the previous mount stale,
 * discarded without write-back) must storage_ext4_cache_flush() its dirty
 * blocks first.  Skipping the flush would silently drop uncommitted writes
 * once the write paths start publishing through this cache. */
int storage_ext4_cache_read(struct storage_volume *volume, uint64_t block, void *out);
/* Journal-owned entries stay pinned after mark_dirty and cache_flush never
 * writes them home. Only journal checkpoint/rollback releases that ownership. */
int storage_ext4_cache_get(struct storage_volume *volume, uint64_t block, uint8_t **data,
                           bool for_write);
int storage_ext4_cache_mark_dirty(struct storage_volume *volume, uint64_t block);
/* Weak: tools/tests/storage_sync_test.c compiles storage_sync.c without
 * linking this cache layer, and the NULL check there keeps that binary
 * linkable; the storage facade always links the real implementation. */
__attribute__((weak)) int storage_ext4_cache_flush(struct storage_volume *volume);
/** @brief Forget a retired volume after the caller settled native journal state.
 * @param volume Quiescent instance or NULL, storage exclusion held. Clears cache
 * flags and borrowed identities; no I/O, page free or independent flush decision.
 */
void storage_ext4_cache_invalidate(struct storage_volume *volume);
/** @brief Check native cache retirement obligations without freeing backing.
 * @return Zero or -EBUSY for dirty/pinned entries, state unchanged. Caller closes
 * provider admission and holds storage execution; no I/O or module callbacks.
 */
int storage_ext4_cache_quiesce(void);
/** @brief Reclaim all shared ext cache domains after all provider users drain.
 * @return Zero, or -EBUSY for any dirty/pinned entry with every allocation
 * retained. Caller holds storage exclusion; no disk I/O, callbacks or module lock.
 */
int storage_ext4_cache_release(void);

uint16_t ext4_get_le16(const uint8_t *p);
uint32_t ext4_get_le32(const uint8_t *p);
uint64_t ext4_get_le64(const uint8_t *p);
void ext4_put_le16(uint8_t *p, uint16_t value);
void ext4_put_le32(uint8_t *p, uint32_t value);
void ext4_put_le64(uint8_t *p, uint64_t value);
int ext4_range_ok(uint64_t raw_len, uint64_t offset, uint64_t size);

int storage_ext4_parse_super(const uint8_t *raw, uint32_t raw_len,
                             struct storage_ext4_super_view *out);
/* Callers must pass the desc_size produced by storage_ext4_parse_super; the
 * 64-bit fields are combined from the hi words iff desc_size >= 64. */
int storage_ext4_parse_group_desc(const uint8_t *raw, uint32_t raw_len,
                                  uint32_t desc_size, struct storage_ext4_group_view *out);
int storage_ext4_parse_extent_header(const uint8_t *raw, uint32_t raw_len,
                                     struct storage_ext4_extent_header_view *out);
uint32_t storage_ext4_crc32c(uint32_t seed, const uint8_t *data, uint32_t len);
uint16_t storage_ext4_crc16(uint16_t seed, const uint8_t *data, uint32_t len);
uint32_t storage_ext4_super_csum_seed(const struct storage_ext4_super_view *view);
int storage_ext4_verify_super_checksum(const uint8_t *sb_raw, uint32_t sb_len);
int storage_ext4_update_super_checksum(uint8_t *sb_raw, uint32_t sb_len);
int storage_ext4_verify_group_checksum(const uint8_t *gd_raw, uint32_t desc_size,
                                       uint32_t group,
                                       const struct storage_ext4_super_view *view);
int storage_ext4_update_group_checksum(uint8_t *gd_raw, uint32_t desc_size,
                                       uint32_t group,
                                       const struct storage_ext4_super_view *view);
int storage_ext4_verify_inode_checksum(const uint8_t *ino_raw,
                                       uint32_t inode_size, uint32_t ino,
                                       uint32_t generation,
                                       const struct storage_ext4_super_view *view);
int storage_ext4_update_inode_checksum(uint8_t *ino_raw, uint32_t inode_size,
                                       uint32_t ino, uint32_t generation,
                                       const struct storage_ext4_super_view *view);
int storage_ext4_read_group(struct storage_volume *volume, uint64_t group,
                            struct storage_ext4_group_view *out);
int storage_ext4_write_group(struct storage_volume *volume, uint64_t group,
                             const struct storage_ext4_group_view *view);
int storage_ext4_alloc_blocks(struct storage_volume *volume, uint64_t goal,
                              uint32_t count, uint64_t *first,
                              uint32_t *allocated);
int storage_ext4_free_blocks(struct storage_volume *volume, uint64_t first,
                             uint32_t count);
int storage_ext4_alloc_inode(struct storage_volume *volume, bool directory,
                             uint64_t *ino);
int storage_ext4_free_inode(struct storage_volume *volume, uint64_t ino,
                            bool directory);


/** @brief Write native ext2 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted provider
 * pin required across all writes; no graph/module lock or pointer retention.
 */
int install_format_ext2(struct install_disk_state *disk,uint64_t start_lba,uint64_t sector_count);
/** @brief Write native ext4 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted provider
 * pin required across all writes; no graph/module lock or pointer retention.
 */
int install_format_ext4(struct install_disk_state *disk,uint64_t start_lba,uint64_t sector_count);
#endif /* RELIEFNT_EXT4_INTERNAL_H */

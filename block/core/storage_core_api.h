/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_CORE_API_H
#define RELIEFNT_STORAGE_CORE_API_H

#include <stddef.h>
#include <stdint.h>
#include <reliefos/fs_abi.h>

#include "storage_types.h"

/**
 * @file storage_core_api.h
 * @brief Private block-core and storage-state interface shared by modules.
 *
 * Filesystem families and transports use this contract for volume selection,
 * bounded sector I/O, cache invalidation and installer access. Hardware register layouts and filesystem-specific APIs are declared
 * in their owning headers.
 */

struct ide_device_info;

/** @brief Snapshot registered disk identities without retaining provider pointers.
 * @param disks Caller array, nullable at zero capacity. @param capacity Entry count.
 * @param out_count Required full matching count. @param installable Exclude RAM media.
 * @return Zero or validation/execution/inventory errno; execution ownership
 * required, no native I/O or code ownership escapes the scalar snapshot.
 */
int storage_inventory_snapshot(struct reliefos_install_disk *disks,uint32_t capacity,
                                  uint32_t *out_count,bool installable);

/** @brief Initialize an exclusive empty whole-device filesystem through its provider.
 * @param volume Zeroed instance with requested volume_id/mount_flags, required.
 * @param disk Inventory identity. @param name Required explicit registered type.
 * @return Zero with ready device/type ownership, or validation/geometry/provider
 * errno after full rollback. Caller owns execution; no graph/registry lock
 * spans native I/O. Source LBA zero is valid and geometry comes from the device.
 */
int storage_mount_whole_filesystem(struct storage_volume *volume,uint32_t disk,const char *name);

/** @brief Retire unpublished boot volumes and reset shared storage bookkeeping.
 * BSP exclusive execution only, before namespace/inode/task users. Retains
 * registered device identities and refreshes scalar inventory without probing
 * hardware. Native destructors precede code puts; no callback/graph lock spans
 * cleanup. No namespace is created and no physical root is selected.
 */
void storage_boot_reset(void);

/**
 * @brief Test whether the current single-CPU read transaction may be retried after yielding.
 * @return True only for an asynchronous context before any write and with at most one CPU.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
bool storage_async_can_yield(void);
const char *storage_transport_name(uint8_t transport);
int gpt_find_esp(void);
/** @brief Parse the borrowed Linux root argument before BSP disk discovery. */
void storage_boot_root_configure(const char *cmdline);
/** @brief Return whether automatic first-disk mounting must be deferred. */
bool storage_boot_root_requested(void);
/** @brief Resolve the requested UUID to a unique disk after discovery/sorting. */
int storage_boot_root_select(void);
/** @brief Enable/disable GUID filtering only around the explicit boot mount. */
void storage_boot_root_filter_enable(bool enabled);
/** @brief Accept a GPT root candidate under the current scoped boot filter. */
bool storage_boot_root_accept_guid(const uint8_t guid[16]);
int storage_devlink_next(uint64_t *cursor, char uuid[37], char target[48]);
int storage_devlink_target(const char *path, char target[48]);
/**
 * @brief Zero a raw installer extent through the common write path.
 * @param disk Borrowed present disk inventory record, stable for the transaction.
 * @param lba First physical 512-byte sector.
 * @param sector_count Number of sectors; zero is a no-op.
 * @return Zero after all batches are cleared or the first write errno; partial completion is possible.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Uses the shared scratch buffer, advances mutation state and invalidates caches before native writes.
 */
int install_clear_sectors(struct install_disk_state *disk, uint64_t lba,
                          uint64_t sector_count);
int install_read_sectors(struct install_disk_state *disk, uint64_t lba,
                         uint32_t sector_count, void *buffer);
int install_write_gpt(struct install_disk_state *disk, uint64_t sector_count,
                      uint64_t *out_esp_lba, uint64_t *out_esp_sectors,
                      uint64_t *out_root_lba, uint64_t *out_root_sectors);
/**
 * @brief Write a raw installer extent in bounded native transport batches.
 * @param disk Required present inventory record, stable for the complete transaction.
 * @param lba First physical 512-byte sector.
 * @param sector_count Complete sector count; zero performs no device write.
 * @param buffer Required readable kernel extent of sector_count times 512 bytes.
 * @return Zero after every batch succeeds, otherwise the first validation or transport errno; earlier batches may already be written.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Invalidates shared caches and latches mutation before I/O. The provider token is held for each batch; caller owns outer disk/format exclusion.
 */
int install_write_sectors(struct install_disk_state *disk, uint64_t lba,
                          uint32_t sector_count, const void *buffer);
/**
 * @brief Query capacity through the serialized native disk transport.
 * @param disk Stable discovered disk record, protected against device removal.
 * @param out_sectors Required capacity output, valid only on success.
 * @return Zero, -EINVAL, -EOPNOTSUPP, -EAGAIN or native I/O errno. Holds transport
 * exclusion across PIO/AHCI command and decode; never publishes CPU DMA addresses.
 */
int storage_install_identify(struct install_disk_state *disk,
                             uint64_t *out_sectors);
/** @brief Diagnose permanent sector errors without losing resumable retry status.
 * @param ret Native sector result; -EAGAIN must remain resumable.
 * @return -EAGAIN unchanged, otherwise -EIO; no allocation, lock or provider call.
 */
int storage_read_failure(int ret);
int storage_acquire_task_io(void);
/**
 * @brief Look up a cached directory record in the currently selected volume.
 * @param directory_cluster Native parent cluster, at least two.
 * @param name Required nonempty terminated lookup name.
 * @param out Optional writable copy of the cached node, assigned only on a hit.
 * @return One for a cache hit, zero for invalid input or a miss.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
int storage_dir_index_lookup(uint32_t directory_cluster, const char *name,
                             struct storage_node *out);
/** @brief Flush one ready instance through its owned block provider.
 * @param volume Borrowed ready instance under storage execution exclusion.
 * @return Zero for tmpfs or provider success, otherwise ENODEV/transport errno.
 */
int storage_flush_volume(const struct storage_volume *volume);
int storage_guid_valid(const uint8_t guid[16]);
/**
 * @brief Identify the reserved FAT-family permissions sidecar using ASCII case folding.
 * @param name Readable terminated basename, nullable.
 * @return One for LEONACL.SYS regardless of ASCII case, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_is_acl_metadata_name(const char *name);
/**
 * @brief Compare two exact byte extents lexicographically.
 * @param a Readable kernel extent of len bytes.
 * @param b Readable kernel extent of len bytes.
 * @param len Number of bytes to compare.
 * @return Zero if equal, otherwise the difference between the first unequal unsigned bytes.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_memcmp(const void *a, const void *b, size_t len);
/** @brief Validate registered 512-byte geometry without requiring write access. */
int storage_prepare_block_disk(struct install_disk_state *disk);
int storage_read_install_disk(const struct install_disk_state *disk,
                              uint64_t lba, uint32_t sector_count,
                              void *buffer);
int storage_read_iso_blocks(uint64_t lba, uint32_t block_count, void *buffer);
/**
 * @brief Read physical sectors through the currently selected volume token.
 * @param lba First physical 512-byte sector.
 * @param sector_count Nonzero complete sector count.
 * @param buffer Writable kernel extent of sector_count times 512 bytes, stable throughout I/O.
 * @return Zero or negative bounds/transport errno, preserving resumable -EAGAIN.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Uses the registered device and transport exclusion; no raw RAM backing or ownership transfer.
 */
int storage_read_sectors(uint64_t lba, uint32_t sector_count, void *buffer);
/**
 * @brief Select a live native volume while preserving the previous selection for restoration.
 * @param node Required borrowed identity whose volume and mount generation remain live.
 * @param old_volume Optional writable previous-selection output; initialized after I/O admission, including before a stale-generation error.
 * @return Zero, -ENOENT for no live volume, -ESTALE for a reused mount, or I/O admission errno.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Non-tmpfs paths may claim task I/O admission. Restore the saved selection on completion; the borrowed node must already retain its mount/provider lifetime.
 */
int storage_select_node_volume(const struct storage_node *node,
                               struct storage_volume **old_volume);
/**
 * @brief Compare two terminated kernel strings byte for byte.
 * @param a Readable terminated kernel string, nullable.
 * @param b Readable terminated kernel string, nullable.
 * @return One if both non-NULL strings match, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_text_eq(const char *a, const char *b);
/**
 * @brief Compare terminated kernel strings with ASCII-only case folding.
 * @param a Readable terminated kernel string, nullable.
 * @param b Readable terminated kernel string, nullable.
 * @return One if both non-NULL strings match, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_text_eq_ci(const char *a, const char *b);
int storage_write_install_disk(const struct install_disk_state *disk,
                               uint64_t lba, uint32_t sector_count,
                               const void *buffer);
/** @brief Write selected-volume sectors through its live registered transport.
 * @param lba First sector. @param sector_count Complete nonzero sector count.
 * @param buffer Stable readable input; caller retains it throughout the call.
 * @return Zero or negative geometry/I/O errno. Latches synchronous mutation,
 * invalidates aliases and retains transport/code ownership through all chunks.
 */
int storage_write_sectors(uint64_t lba, uint32_t sector_count,
                          const void *buffer);
/**
 * @brief Measure a nullable terminated kernel string.
 * @param s Readable NUL-terminated kernel string, nullable.
 * @return Bytes before NUL, or zero for NULL.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
size_t storage_strlen(const char *s);
/**
 * @brief Choose the smaller unsigned 32-bit scalar.
 * @param a First value.
 * @param b Second value.
 * @return The smaller argument, unchanged.
 * @par Context
 * Pure scalar operation; any context and no ownership transfer.
 */
uint32_t min_u32(uint32_t a, uint32_t b);
uint32_t storage_crc32(const void *data, uint32_t len);
/**
 * @brief Translate an allocated FAT-family data cluster using the selected volume geometry.
 * @param cluster Validated data cluster at least two, within the selected filesystem.
 * @return Physical sector address computed from the selected partition and data-area offsets.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. No I/O, allocation, range validation or reference acquisition.
 */
uint64_t cluster_to_lba(uint32_t cluster);
/**
 * @brief Locate the FAT32 sector containing an allocated cluster entry.
 * @param cluster Validated cluster index within the selected FAT.
 * @return Physical sector address of its four-byte FAT entry.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Geometry, including nonzero bytes_per_sector, must already be validated.
 */
uint64_t fat_sector_for_cluster(uint32_t cluster);
/**
 * @brief Choose the smaller unsigned 64-bit scalar.
 * @param a First value.
 * @param b Second value.
 * @return The smaller argument, unchanged.
 * @par Context
 * Pure scalar operation; any context and no ownership transfer.
 */
uint64_t min_u64(uint64_t a, uint64_t b);
/** @brief Compute the same block or anonymous device identity for stat/mountinfo.
 * @param volume Required stable native instance, borrowed under execution ownership.
 * @return Registered block dev_t when known, otherwise volume_id+1 anonymous
 * identity. Pure geometry/state query, no I/O, allocation or reference transfer.
 */
uint64_t storage_volume_device_number(const struct storage_volume *volume);
/**
 * @brief Advance the metadata epoch and make the current asynchronous transaction non-replayable.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_begin_mutation(void);
/** @brief Retire native metadata/physical caches and advance the mutation epoch.
 * @par Context Caller holds storage execution ownership; no callback/allocation.
 */
void storage_cache_invalidate(void);
/**
 * @brief Copy and terminate a bounded kernel string, truncating when necessary.
 * @param dst Writable cap-byte kernel buffer, nullable for a no-op.
 * @param cap Buffer capacity including its terminator; zero is a no-op.
 * @param src Readable terminated string, nullable for empty output.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_copy_text(char *dst, uint32_t cap, const char *src);
/**
 * @brief Publish a copied directory record in the bounded replacement cache.
 * @param directory_cluster Native parent cluster, at least two.
 * @param name Required nonempty terminated name, copied into the cache.
 * @param node Required borrowed live metadata, copied by value.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_dir_index_store(uint32_t directory_cluster, const char *name,
                             const struct storage_node *node);
void storage_format_guid(const uint8_t guid[16], char *out, uint32_t cap);
void storage_make_guid(uint8_t guid[16], uint64_t tag,
                       const struct install_disk_state *disk,
                       uint64_t sector_count);
void storage_partition_guid_text(const uint8_t guid[16], char uuid[37]);
/**
 * @brief Restore a previously borrowed native volume selection.
 * @param old_volume Previous live selection from storage_select_node_volume; NULL leaves selection unchanged.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_restore_volume(struct storage_volume *old_volume);
/** @brief Retire block/directory caches and advance the mutation epoch.
 * @par Context Caller holds storage execution ownership; no callback/allocation.
 */
void storage_sector_cache_invalidate(void);
/** @brief Acquire a fresh volume's exact registered block-device ownership.
 * @param volume Exclusive instance with empty token, required.
 * @param disk Borrowed scalar inventory record, required.
 * @return Zero or -EINVAL/-EBUSY/lookup errno; failure retains no reference.
 * Execution ownership required; copies geometry without native callbacks/I/O.
 */
int storage_volume_from_install_disk(struct storage_volume *volume,const struct install_disk_state *disk);
/** @brief Consume a quiescent volume's device token after native/cache teardown.
 * @param volume Exclusive retired instance, nullable. Execution ownership required;
 * no callback/I/O; never use this to release a copied ownership token.
 */
void storage_volume_put_device(struct storage_volume *volume);

/** @brief Snapshot admitted disk metadata without native pointers or self-pins.
 * Execution ownership required; absent identities remain reserved and late
 * loads append without renaming physical endpoints. No native callbacks or I/O.
 * @return Zero or metadata allocation/encoded-ID exhaustion errno; existing
 * slots remain valid, including on failed growth.
 */
int storage_refresh_disks(void);

/** @brief Enumerate scalar optical IDs without requiring filesystem code/media I/O.
 * @param cursor Required monotonic block cursor. @param disk_id Output node ID.
 * @return One, zero at end, or negative errno. Execution ownership, temporary
 * exact block/code pin consumed before return; no callbacks or allocation.
 */
int storage_optical_next(uint64_t *cursor,uint32_t *disk_id);
/** @brief Read a short optical byte range through current owned staging.
 * @param ref Borrowed exact block reference. @param offset Byte offset.
 * @param buffer Writable length bytes. @param length Maximum bytes.
 * @param out_read Required completed-byte output, cleared on entry.
 * @return Zero or native errno including EAGAIN. Execution ownership; acquires
 * generic task I/O admission and transport lock, never DMA to task memory.
 */
int storage_optical_read(const struct block_device_ref *ref,uint64_t offset,void *buffer,uint32_t length,uint32_t *out_read);

/**
 * @brief Zero exactly the requested byte extent.
 * @param dst Writable kernel extent of len bytes; nullable only for zero length.
 * @param len Number of bytes to clear.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_memzero(void *dst, size_t len);

/**
 * @brief Copy an exact nonoverlapping byte extent without allocating.
 * @param dst Writable kernel extent of len bytes.
 * @param src Readable kernel extent of len bytes, not overlapping dst.
 * @param len Number of bytes to copy.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_memcpy(void *dst, const void *src, size_t len);

/** @brief Read bounded sectors through the instance's exact block reference.
 * @param volume Borrowed pinned instance. @param lba First sector.
 * @param sector_count Nonzero sectors. @param buffer Current writable output.
 * @return Zero or transport/bounds errno, preserving EAGAIN. Execution ownership
 * required; transport exclusion spans callbacks and no raw backing is exposed.
 */
int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                        uint32_t sector_count, void *buffer);

/** @brief Write bounded sectors through a live provider after alias invalidation.
 * @param volume Borrowed pinned instance. @param lba First sector.
 * @param sector_count Nonzero sectors. @param buffer Current readable input.
 * @return Zero or transport/bounds errno; no incomplete transfer is success.
 * Caller owns execution exclusion; provider code stays pinned during callbacks.
 */
int storage_write_device(const struct storage_volume *volume, uint64_t lba,
                         uint32_t sector_count, const void *buffer);

#endif /* RELIEFNT_STORAGE_CORE_API_H */

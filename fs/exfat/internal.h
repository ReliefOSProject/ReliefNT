#ifndef RELIEFNT_FS_EXFAT_EXFAT_C_INTERNAL_H
#define RELIEFNT_FS_EXFAT_EXFAT_C_INTERNAL_H

#include "../../block/core/storage_internal.h"
/*
 * exFAT storage backend.
 *
 * This deliberately keeps filesystem code independent of the transport.  All
 * media accesses below use storage_read_sectors()/storage_write_sectors(), so
 * AHCI, IDE PIO and NVMe share one locking and recovery path.
 */




/* The upcase stream is at most 65536 UTF-16 code units (the standard table
 * is much smaller).  Keep one raw copy while mounting so decoding never
 * performs hundreds of tiny device reads against a freshly formatted target.
 * The buffer is also aligned for transports which require DMA-safe memory. */


/* exFAT metadata is accessed much more frequently than ordinary file data:
 * directory walks follow FAT links and allocation updates touch one bit at a
 * time. Keep a small write-through cache for each metadata stream. The cache
 * is deliberately private to exFAT so the generic FAT32 cache invalidation
 * cannot evict it halfway through a cluster allocation transaction. */
#define EXFAT_META_CACHE_SECTORS 8u
#define EXFAT_READ_BATCH_SECTORS 8u















struct exfat_chain_hint {
    struct storage_volume *volume;
    uint32_t first;
    uint32_t cluster;
    uint64_t index;
    uint8_t nofat;
    uint8_t valid;
};










/*
 * AHCI has an optional syscall-level asynchronous path.  A filesystem read
 * can, however, consist of several transport requests (for example a read
 * crossing a cluster boundary).  Replaying the syscall from its beginning
 * while a later request is pending makes those requests alternate forever:
 * the caller asks for request A while the controller is still completing B.
 * Keep exFAT's metadata and stream operations atomic with respect to that
 * retry protocol.  The transport still serializes access and has bounded
 * waits; only the restartable async shortcut is disabled for this operation.
 */




/* Keep metadata reads within the same small request size that the installer
 * transport has already proven reliable.  A failed multi-sector request is
 * retried one sector at a time so a transient AHCI/NVMe/IDE completion error
 * does not turn a valid directory into a metadata-corruption report. */


/* Some legacy AHCI implementations intermittently reject a multi-sector
 * write while accepting the exact same sectors as individual commands.  The
 * filesystem must not lose an installation because of that transport quirk:
 * retry the request one sector at a time after the normal transport retry
 * path has failed.  Each sector write is idempotent, so this is safe even if
 * the controller completed part of the original request before reporting an
 * error. */



/* Byte-exact native create/modify/access fields, increments and UTC offsets. */
struct exfat_entry_timestamps {uint8_t bytes[17];};

struct exfat_dir_ref {
    uint32_t directory_cluster;
    uint32_t first_entry;
    uint8_t directory_nofat;
    uint8_t secondary_count;
};






























/* System directory entries (allocation bitmap and upcase table) do not have
 * a stream-extension flags byte.  Implementations commonly store them as a
 * contiguous extent with FAT[first] == EOC, while the installer formatter
 * uses an ordinary FAT chain.  Detect the representation from the on-disk
 * chain and reject ambiguous/corrupt layouts rather than assuming one form. */








/* Read arbitrary bytes from an allocated exFAT cluster stream. */


/* Write arbitrary bytes into already-reserved clusters. */




























/* Merge a committed sector back into the directory window when it is still
 * resident. This prevents a later lookup from resurrecting stale entries. */








/* Return the file-entry reference for the final component as well as its
 * node.  Root has no file entry set, which is represented by a null ref. */


























/* Directories created by ReliefOS always use FAT chains.  Extending one must
 * update the owning stream extension too; otherwise standard fsck tools see
 * the new cluster as an orphaned allocation. */














/* ACL metadata is hidden from normal directory listings, so it must not keep
 * a directory artificially non-empty or leak its clusters after rmdir. */


extern uint8_t storage_exfat_upcase_ready[STORAGE_MAX_VOLUMES];
extern struct exfat_chain_hint exfat_chain_hint;
extern struct storage_volume *exfat_dir_cache_volume;
extern uint32_t exfat_dir_cache_cluster;
extern uint64_t exfat_dir_cache_offset;
extern uint32_t exfat_dir_cache_bytes;
extern uint8_t exfat_dir_cache_valid;

int exfat_read_sectors_resilient(uint64_t lba, uint32_t sectors,
                                        uint8_t *data);

int exfat_write_sectors_resilient(uint64_t lba, uint32_t sectors,
                                         const uint8_t *data);

void exfat_cache_invalidate(void);

uint16_t exfat_get_u16(const uint8_t *p);

void exfat_put_u16(uint8_t *p, uint16_t value);

uint64_t exfat_get_u64(const uint8_t *p);

void exfat_put_u64(uint8_t *p, uint64_t value);

int exfat_cluster_valid(uint32_t cluster);

uint64_t exfat_cluster_lba(uint32_t cluster);

uint16_t exfat_checksum(const uint8_t *entries, uint32_t count);

int exfat_boot_region_valid(uint64_t base_lba, uint8_t boot_sector[SECTOR_SIZE]);

int exfat_read_fat(uint32_t cluster, uint32_t *next);

int exfat_write_fat(uint32_t cluster, uint32_t next);

int exfat_cluster_at(uint32_t first, uint8_t nofat, uint64_t index,
                            uint32_t *out_cluster);

int exfat_system_stream_mode(uint32_t first, uint64_t length, uint8_t *out_nofat);

int exfat_validate_fat_chain(uint32_t first);

int exfat_fat_chain_contains(uint32_t first, uint32_t wanted);

int exfat_stream_contains_cluster(uint32_t first, uint8_t nofat,
                                          uint64_t length, uint32_t wanted);

int exfat_read_stream(uint32_t first, uint8_t nofat, uint64_t offset,
                             void *buffer, uint32_t len);

int exfat_write_stream(uint32_t first, uint8_t nofat, uint64_t offset,
                              const void *buffer, uint32_t len);

int exfat_bitmap_flush(void);

int exfat_bitmap_get_cached(uint32_t cluster, uint8_t *allocated);

int exfat_bitmap_set_cached(uint32_t cluster, uint8_t allocated);

int exfat_read_bitmap_bit(uint32_t cluster, uint8_t *allocated);

int exfat_load_upcase_table(void);

uint16_t exfat_name_hash(const uint16_t *name, uint32_t length);

int exfat_utf8_name(const char *name, uint16_t out[255], uint32_t *out_length);

int exfat_name_equals(const uint16_t *name, uint32_t length, const char *other);

int exfat_dir_read_entry(uint32_t directory_cluster, uint8_t nofat,
                                uint32_t index, uint8_t entry[EXFAT_ENTRY_SIZE]);

uint64_t exfat_dir_entry_limit(void);

int exfat_find_in_dir_ref(uint32_t directory_cluster, uint8_t nofat, const char *wanted,
                                 struct storage_node *out, struct exfat_dir_ref *out_ref);



int exfat_read_node(const struct storage_node *node, uint64_t offset,
                           void *buffer, uint32_t len, uint32_t *out_read);

int exfat_write_contiguous_fat(uint32_t first, uint32_t count, uint32_t next_after);

int exfat_iter_dir_entry(uint32_t directory_cluster, uint8_t nofat, uint64_t wanted,
                                struct reliefos_dir_entry *out);

int exfat_zero_clusters(uint32_t first, uint8_t nofat, uint32_t count);

int exfat_allocate_clusters(uint32_t count, uint32_t *out_first, uint8_t *out_nofat);

int exfat_free_clusters(uint32_t first, uint8_t nofat, uint64_t bytes);

int exfat_create_entry(uint32_t directory_cluster, uint8_t directory_nofat,
                              const struct exfat_dir_ref *self_ref, const char *name,
                              uint8_t directory, uint32_t first_cluster, uint64_t size,
                              uint8_t nofat, struct exfat_dir_ref *out_ref);

int exfat_update_entry_data(const struct exfat_dir_ref *ref, uint32_t first_cluster,
                                   uint64_t size, uint8_t nofat);


int exfat_mkdir_child(const struct storage_node *parent,const char *name,struct storage_node *out);
int exfat_node_ref(const struct storage_node *node,struct exfat_dir_ref *out);
int exfat_remove_child(const struct storage_node *parent,const char *name,bool directory);

int exfat_delete_entry(const struct exfat_dir_ref *ref);

int exfat_dir_is_empty(const struct storage_node *node);




int exfat_mount(void);

/** @brief Rename between explicit retained exFAT parent directories.
 * @param directory Required source directory. @param old_name Literal source leaf.
 * @param destination Required target directory. @param new_name Validated target leaf.
 * @param committed Required irreversible-publication output, initially false.
 * @return Zero or negative native validation/metadata/I/O errno. Errors after
 * publication retain committed=true. Both rename branches preserve all native
 * timestamps; replacement failures restore old target data/time records.
 * Caller owns execution, source/target/code pins and pre-reserved mutation
 * resources. Timestamp transfer uses stack storage; native directory growth
 * follows its existing reservation rules. No global path/callback locks.
 */
int exfat_rename_child(const struct storage_node *parent,const char *old_name,
                          const struct storage_node *destination,const char *new_name,bool *committed);
/** @brief Decode native exFAT access/modification metadata for one held inode.
 * @param node Required linked node in the selected volume. Roots have no record.
 * @param out Required output, cleared on failure; invalid time fields remain unknown.
 * @return Zero or negative structural/record/I/O errno. Native UTC offsets and
 * modification10ms fields are respected; initial ctime follows mtime because
 * the format stores birth time separately, not a persistent POSIX change time.
 * Execution ownership required; no graph/module lock or ownership transfer.
 */
int exfat_inode_times(const struct storage_node *node,struct storage_inode_times *out);
/** @brief Update one current exFAT primary entry's UTC access time and checksum.
 * @param node Required linked node in the selected volume; roots have no record.
 * @param seconds Current UTC Unix seconds, clamped to the native date range.
 * @param actual Required normalized even-second output, unchanged on failure.
 * @return Zero or negative validation/metadata/I/O errno. Only the primary
 * entry changes, preserving stream/name records. Equal encoded fields avoid
 * writes. Caller owns execution, synchronous I/O and a retained inode/code pin;
 * orphans are excluded by the caller, no global pathname or callback locks.
 */
int exfat_inode_access(const struct storage_node *node,int64_t seconds,int64_t *actual);
/** @brief Read the checksum-validated native time bytes of one exFAT entry set.
 * @param ref Required selected-directory location; never a global name.
 * @param out Required byte-record output, cleared on failure.
 * @return Zero or negative metadata/structure/I/O errno. Execution ownership and
 * retained source/target identities are required; no allocation, callback lock
 * or ownership transfer. All creation/modification/access bytes are preserved.
 */
int exfat_entry_times_read(const struct exfat_dir_ref *ref,struct exfat_entry_timestamps *out);
/** @brief Write only native exFAT time bytes and their primary-entry checksum.
 * @param ref Required selected-directory location, held stable by the caller.
 * @param value Required borrowed byte-exact time record.
 * @return Zero or negative metadata/structure/I/O errno. Unchanged bytes avoid
 * writes; a changed primary entry is published under the mutation latch. Caller
 * owns synchronous execution and retained inode/code identities; no allocation
 * or graph/module callback lock. Stream/name bytes and unrelated metadata stay.
 */
int exfat_entry_times_write(const struct exfat_dir_ref *ref,const struct exfat_entry_timestamps *value);

/** @brief Write native exfat metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted provider
 * pin required across all writes; no graph/module lock or pointer retention.
 */
int install_format_exfat(struct install_disk_state *disk,uint64_t start_lba,uint64_t sector_count);
#endif

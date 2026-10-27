#ifndef RELIEFNT_FS_FAT_FAT32_C_INTERNAL_H
#define RELIEFNT_FS_FAT_FAT32_C_INTERNAL_H

#include "../../block/core/storage_internal.h"




int fat32_mount(void);

int fat32_read_fat_entry(uint32_t cluster, uint32_t *out_next);

int storage_read_cache_lookup(uint32_t cluster, uint32_t *out_offset,
                                     uint32_t *out_clusters);

int storage_read_contiguous_clusters(uint32_t first_cluster,
                                            uint32_t max_clusters,
                                            uint32_t *out_clusters,
                                            uint32_t *out_next);

int storage_read_cache_fill(uint32_t first_cluster, uint32_t clusters);

int fat32_read_lookup_cluster(uint32_t cluster, void *buffer);

int fat32_write_fat_entry(uint32_t cluster, uint32_t value);

int fat32_note_allocated(void);

int fat32_note_freed(void);

int fat32_read_cluster(uint32_t cluster, void *buffer);

int fat32_write_cluster(uint32_t cluster, const void *buffer);

int fat32_make_short_name(const char *name, uint8_t out[11]);

int fat32_find_free_cluster(uint32_t *out_cluster);

int fat32_collect_chain(uint32_t first_cluster, uint32_t chain[FAT32_MAX_FILE_CLUSTERS], uint32_t *out_count);

int fat32_free_chain(uint32_t first_cluster);

int fat32_name_match_short(const struct fat32_dirent *de, const char *name);

void fat32_lfn_extract_utf16(const struct fat32_lfn *lfn, uint16_t *dst,
                                    uint32_t *len, uint32_t cap);

void fat32_build_lfn_name(const uint16_t lfn_parts[20][13], uint32_t lfn_count,
                                 char *dst, uint32_t cap);

int fat32_validate_name(const char *name);

int fat32_make_short_alias(uint32_t dir_cluster, const char *name, uint8_t out[11]);

uint8_t fat32_short_name_checksum(const uint8_t short_name[11]);

uint32_t fat32_utf16_name(const char *name, uint16_t *utf16, uint32_t cap);

uint32_t fat32_lfn_entry_count(const char *name);

void fat32_fill_lfn_entry(struct fat32_lfn *lfn,
                                 const uint16_t *name, uint32_t name_len,
                                 uint32_t part_index, uint32_t part_count,
                                 uint8_t checksum);

int fat32_find_in_dir(uint32_t dir_cluster, const char *name, struct storage_node *out);

int fat32_find_dirent_ref_in_dir(uint32_t dir_cluster, const char *name, struct fat32_dir_ref *out);

int fat32_update_dirent(const struct fat32_dir_ref *ref);

int fat32_name_needs_lfn(const char *name, uint8_t short_name[11], uint8_t *need_lfn);

int fat32_find_free_dirent_span(uint32_t dir_cluster, uint32_t slots,
                                       struct fat32_dir_span *out);

int fat32_create_dirent(uint32_t parent_cluster, const char *name, uint8_t attr,
                               uint32_t first_cluster, uint32_t size);

int fat32_delete_dirent(uint32_t dir_cluster, const char *name,
                               struct fat32_dirent *deleted);

int fat32_delete_acl_metadata_file(uint32_t dir_cluster);

int fat32_dir_is_empty(uint32_t dir_cluster);

int fat32_iter_dir_entry(uint32_t dir_cluster, uint64_t index, struct reliefos_dir_entry *entry);

int fat32_mkdir_child(const struct storage_node *parent,const char *name,struct storage_node *out);
int fat32_remove_child(const struct storage_node *parent,const char *name,bool directory);

/** @brief Rename between explicit retained FAT parent directories.
 * @param parent Required current source directory. @param old_name Literal source leaf.
 * @param destination Required target directory. @param new_name Validated target leaf.
 * @param committed Required irreversible-publication output, initially false.
 * @return Zero or negative native metadata/validation/I/O errno. Errors after
 * publication retain committed=true. Source times survive both new-target and
 * replacement renames; failed publication restores the original target record.
 * Caller owns execution and all inode/code pins, checks mountpoint/ancestry and
 * reserves mutation resources. No global path resolution or callback locks.
 */
int fat32_rename_child(const struct storage_node *parent,const char *old_name,
                          const struct storage_node *destination,const char *new_name,bool *committed);
/** @brief Decode native FAT modification/access fields without a pathname.
 * @param node Borrowed retained node in the selected live FAT volume.
 * @param out Required output, cleared on failure; invalid date fields stay unknown.
 * @return Zero or negative record/I/O errno. Roots have no persistent timestamp.
 * FAT has no stored status-change time; initial ctime follows native mtime,
 * matching the Linux FAT inode view. Execution ownership; no reference transfer.
 */
int fat32_inode_times(const struct storage_node *node,struct storage_inode_times *out);
/** @brief Update only a retained FAT inode's access-date field in its current record.
 * @param node Required linked node in the selected volume; roots have no record.
 * @param seconds Current UTC seconds, clamped to the native1980..2107 date range.
 * @param actual Required normalized midnight-seconds output, unchanged on failure.
 * @return Zero or negative record/I/O errno. Execution ownership/synchronous I/O
 * required. Unchanged dates avoid writes; mutation is latched before updating.
 * Caller owns the inode pin and excludes orphans from all directory-record I/O.
 */
int fat32_inode_access(const struct storage_node *node,int64_t seconds,int64_t *actual);

/** @brief Write native fat32 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted provider
 * pin required across all writes; no graph/module lock or pointer retention.
 */
int install_format_fat32(struct install_disk_state *disk,uint64_t start_lba,uint64_t sector_count);
#endif

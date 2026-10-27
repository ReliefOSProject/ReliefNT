/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_INTERNAL_H
#define RELIEFNT_FS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <reliefnt/storage.h>
#include <reliefnt/text_utf16.h>

#include "../block/core/storage_types.h"
#include "ext_common/internal.h"
#include "cluster_time.h"

/**
 * @file fs_internal.h
 * @brief Private on-disk types and filesystem-family interfaces.
 */

#define FAT32_EOC 0x0ffffff8u
#define FAT32_ATTR_DIRECTORY 0x10u
#define FAT32_ATTR_VOLUME 0x08u
#define FAT32_ATTR_LFN 0x0fu
#define FAT32_ATTR_HIDDEN 0x02u
#define FAT32_ATTR_SYSTEM 0x04u
#define FAT32_ATTR_ARCHIVE 0x20u
#define FAT32_DIR_ENTRY_BYTES 32u

#define ISO9660_PVD_LBA 16u
#define EXT2_SUPERBLOCK_OFFSET 1024u
#define EXT2_SUPER_MAGIC 0xef53u
#define EXT2_ROOT_INO 2u
#define EXT2_GOOD_OLD_REV 0u
#define EXT2_DYNAMIC_REV 1u
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT2_S_IFMT 0xf000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u
#define EXT2_S_IFLNK 0xa000u
#define EXT2_FT_UNKNOWN 0u
#define EXT2_FT_REG_FILE 1u
#define EXT2_FT_DIR 2u
#define EXT2_FT_SOCK 6u
#define EXFAT_EOC 0xffffffffu
#define EXFAT_ENTRY_SIZE 32u
#define EXFAT_ENTRY_FILE 0x85u
#define EXFAT_ENTRY_STREAM 0xc0u
#define EXFAT_ENTRY_NAME 0xc1u
#define EXFAT_ENTRY_BITMAP 0x81u
#define EXFAT_ENTRY_UPCASE 0x82u
#define EXFAT_ATTR_DIRECTORY 0x0010u
#define EXFAT_STREAM_NO_FAT_CHAIN 0x02u

struct __attribute__((packed)) fat32_bpb {
    uint8_t jump[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors16;
    uint8_t media;
    uint16_t fat_size16;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
};

struct __attribute__((packed)) fat32_dirent {
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t size;
};

struct __attribute__((packed)) fat32_lfn {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
};

struct __attribute__((packed)) ext2_superblock {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    int32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    int16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
};

struct __attribute__((packed)) ext2_group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
};

struct __attribute__((packed)) ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_512;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t size_high;
    uint32_t faddr;
    uint8_t osd2[12];
};

struct __attribute__((packed)) ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
};

struct fat32_dir_ref {
    uint32_t entry_cluster;
    uint32_t entry_offset;
    struct fat32_dirent dirent;
};

struct fat32_dir_span {
    uint32_t entry_cluster;
    uint32_t entry_offset;
};

struct storage_inode_ref {
    struct storage_inode_ref *next;
    struct storage_node node;
    uint64_t references;
    /* Positive writable OFDs, negative kernel read guards; execution-owned. */
    int64_t write_count;
    bool read_only;
    bool unlinked;
    /* FAT-family inode metadata survives renamed/unlinked directory records.
     * Shared by all aliases; first population uses a held parent directory. */
    bool permissions_valid;
    struct reliefos_permissions permissions;
    bool times_valid;
    struct storage_inode_times times;
};
/** @brief Intern a FAT/exFAT directory record with stable per-mount inode identity.
 * @param node Borrowed metadata, updated with a unique cache generation.
 * @return Zero or negative errno. Execution transaction required. Unused cache
 * entries may be reclaimed; held entries have no fixed capacity cutoff.
 */
int storage_inode_cache_node(struct storage_node *node);
/** @brief Publish committed cluster/size changes to every held inode alias.
 * @param before Original identity, borrowed.
 * @param after Committed backing metadata, borrowed; identity stays unchanged.
 */
void storage_inode_note_data(const struct storage_node *before,const struct storage_node *after);
/** @brief Disconnect a committed directory removal while deferring held chains.
 * @param node Removed identity, borrowed under execution ownership.
 * @return True if final inode put owns chain destruction; false if caller does.
 */
bool storage_inode_unlinked(const struct storage_node *node);
bool storage_inode_is_unlinked(const struct storage_node *node);
/** @brief Transfer a renamed FAT/exFAT inode to its committed directory slot.
 * @param before Original stable identity, borrowed.
 * @param after New directory-record location and metadata, borrowed.
 */
void storage_inode_note_move(const struct storage_node *before,const struct storage_node *after);
/** @brief Discard unused cluster inode cache records before volume reuse.
 * @param volume Backend slot, under execution ownership.
 * @return Zero or EBUSY if a held/failed-retirement inode prevents reuse.
 */
int storage_inode_purge_volume(uint32_t volume);
/** @brief Reject direct kernel mutations while a module snapshot denies writes.
 * @param node Existing identity, borrowed under execution ownership.
 * @return Zero or ETXTBSY; does not acquire an OFD writer lease.
 */
int storage_inode_mutation_allowed(const struct storage_node *node);
int storage_cluster_write(struct storage_node *node,uint64_t offset,const void *data,
                             uint32_t length,uint32_t *written);
int storage_cluster_truncate(struct storage_node *node,uint64_t length);
/* Filesystem interface (fs/): nodes, directories, paths, formats. */
bool storage_inode_volume_busy(uint32_t volume); /* fs/vfs/inode.c */
int ed_stop(struct storage_ext4_handle *h,int ret); /* fs/ext_common/dir.c */
int exfat_bitmap_get_cached(uint32_t cluster, uint8_t *allocated); /* fs/exfat/exfat_cache.c */
int exfat_iter_dir_entry(uint32_t directory_cluster, uint8_t nofat, uint64_t wanted, struct reliefos_dir_entry *out); /* fs/exfat/exfat_dir.c */
int exfat_read_node(const struct storage_node *node, uint64_t offset, void *buffer, uint32_t len, uint32_t *out_read); /* fs/exfat/exfat_file.c */
int ext2_cache_read(uint64_t lba, uint32_t sectors, void *buffer); /* fs/ext_common/ext2_cache.c */
int ext4_cache_block_range(const struct storage_volume *volume, uint64_t block, uint64_t *out_lba, uint32_t *out_sectors); /* fs/ext_common/cache.c */
int fat32_collect_chain(uint32_t first_cluster, uint32_t chain[FAT32_MAX_FILE_CLUSTERS], uint32_t *out_count); /* fs/fat/fat32_alloc.c */
int fat32_create_dirent(uint32_t parent_cluster, const char *name, uint8_t attr, uint32_t first_cluster, uint32_t size); /* fs/fat/fat32_dir.c */
int fat32_delete_acl_metadata_file(uint32_t dir_cluster); /* fs/fat/fat32_dir.c */
int fat32_delete_dirent(uint32_t dir_cluster, const char *name, struct fat32_dirent *deleted); /* fs/fat/fat32_dir.c */
int fat32_dir_is_empty(uint32_t dir_cluster); /* fs/fat/fat32_dir.c */
int fat32_find_dirent_ref_in_dir(uint32_t dir_cluster, const char *name, struct fat32_dir_ref *out); /* fs/fat/fat32_dir.c */
int fat32_find_free_cluster(uint32_t *out_cluster); /* fs/fat/fat32_alloc.c */
int fat32_find_free_dirent_span(uint32_t dir_cluster, uint32_t slots, struct fat32_dir_span *out); /* fs/fat/fat32_dir.c */
int fat32_find_in_dir(uint32_t dir_cluster, const char *name, struct storage_node *out); /* fs/fat/fat32_dir.c */
int fat32_free_chain(uint32_t first_cluster); /* fs/fat/fat32_alloc.c */
int fat32_iter_dir_entry(uint32_t dir_cluster, uint64_t index, struct reliefos_dir_entry *entry); /* fs/fat/fat32_dir.c */
int fat32_make_short_alias(uint32_t dir_cluster, const char *name, uint8_t out[11]); /* fs/fat/fat32_name.c */
int fat32_make_short_name(const char *name, uint8_t out[11]); /* fs/fat/fat32_name.c */
int fat32_note_allocated(void); /* fs/fat/fat32_io.c */
int fat32_note_freed(void); /* fs/fat/fat32_io.c */
int fat32_read_cluster(uint32_t cluster, void *buffer); /* fs/fat/fat32_io.c */
int fat32_read_fat_entry(uint32_t cluster, uint32_t *out_next); /* fs/fat/fat32_io.c */
int fat32_update_dirent(const struct fat32_dir_ref *ref); /* fs/fat/fat32_dir.c */
int fat32_validate_name(const char *name); /* fs/fat/fat32_name.c */
int fat32_write_cluster(uint32_t cluster, const void *buffer); /* fs/fat/fat32_io.c */
int fat32_write_fat_entry(uint32_t cluster, uint32_t value); /* fs/fat/fat32_io.c */
int iso9660_find_in_dir(uint32_t extent, uint32_t size, const char *name, struct storage_node *out); /* fs/iso9660/iso.c */
int iso9660_iter_dir_entry(uint32_t extent, uint32_t size, uint64_t index, struct reliefos_dir_entry *entry); /* fs/iso9660/iso.c */
int iso9660_mount(void); /* fs/iso9660/iso.c */
int storage_read_cache_fill(uint32_t first_cluster, uint32_t clusters); /* fs/fat/fat32_io.c */
int storage_read_cache_lookup(uint32_t cluster, uint32_t *out_offset, uint32_t *out_clusters); /* fs/fat/fat32_io.c */
int storage_read_contiguous_clusters(uint32_t first_cluster, uint32_t max_clusters, uint32_t *out_clusters, uint32_t *out_next); /* fs/fat/fat32_io.c */
uint32_t ext4_base_meta_blocks(const struct storage_volume *volume, uint64_t group); /* fs/ext_common/alloc.c */
uint32_t fat32_lfn_entry_count(const char *name); /* fs/fat/fat32_name.c */
uint32_t fat32_utf16_name(const char *name, uint16_t *utf16, uint32_t cap); /* fs/fat/fat32_name.c */
uint8_t fat32_short_name_checksum(const uint8_t short_name[11]); /* fs/fat/fat32_name.c */
/** @brief Classify a decoded ext-family inode mode without allocation or I/O.
 * @param mode Linux inode type/mode bits.
 * @return Existing native directory/link/FIFO/socket class or regular default.
 */
uint32_t storage_ext4_inode_type(uint16_t mode);
/** @brief Construct node metadata directly from a decoded/native-created inode.
 * @param v Borrowed mounted volume with stable instance generation.
 * @param ino Native inode number belonging to the supplied volume.
 * @param in Borrowed inode view, no read or ownership transfer performed.
 * @param out Optional writable node metadata output, not a held inode pin.
 * Caller serializes publication; this low-level helper does not depend on VFS.
 */
void storage_ext4_node_from_inode(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,struct storage_node *out); /* fs/ext_common/ops.c */
void exfat_put_u16(uint8_t *p, uint16_t value); /* fs/exfat/exfat_name.c */
void exfat_put_u64(uint8_t *p, uint64_t value); /* fs/exfat/exfat_name.c */
void ext2_cache_invalidate_range(uint64_t lba, uint32_t sectors); /* fs/ext_common/ext2_cache.c */
/** @brief Forget clean cache entries and borrowed identities for a retired volume.
 * @param volume Quiescent instance; caller holds storage exclusion. Shared cache
 * backing remains owned until final provider teardown, no I/O or allocation.
 */
void ext2_cache_reset(struct storage_volume *volume); /* fs/ext_common/ext2_cache.c */
/** @brief Cache clean write-through sector bytes in owned lazy physical backing.
 * @param lba First native sector. @param sectors Nonzero sectors, at most 8.
 * @param buffer Readable sectors*512 kernel bytes, required for a valid extent.
 * Storage exclusion required. OOM/map failure disables caching with no leaked run.
 */
void ext2_cache_store(uint64_t lba, uint32_t sectors, const void *buffer); /* fs/ext_common/ext2_cache.c */
/** @brief Free the shared clean ext block cache after all provider users drain.
 * @return Zero; storage exclusion required, no I/O. Clears borrowed identities
 * and physical ownership, including on repeated/empty release.
 */
int ext2_cache_release(void);
void ext4_cache_note_checksum_ok(struct storage_volume *volume, uint64_t block); /* fs/ext_common/cache.c */
void fat32_fill_lfn_entry(struct fat32_lfn *lfn, const uint16_t *name, uint32_t name_len, uint32_t part_index, uint32_t part_count, uint8_t checksum); /* fs/fat/fat32_name.c */
/**
 * @brief Format a bounded diagnostic prefix and unsigned disk/partition number.
 * @param out Writable output, ignored if NULL. @param cap Byte capacity including NUL.
 * @param prefix Borrowed optional literal prefix.
 * @param value Unsigned diagnostic number. @param partition Negative to omit the
 * partition suffix, otherwise zero-based index rendered as p(index+1).
 * @return None. Truncates with a terminator whenever out/cap permit; no allocation,
 * I/O or reference transfer. Caller owns its buffer; no shared mutable state.
 */
void storage_format_u32(char *out, uint32_t cap, const char *prefix, uint32_t value, int partition); /* fs/vfs/storage_status.c */
int ext2_mount(void); /* fs/ext_common/ext2.c */
int ext2_group_desc(uint32_t group, struct ext2_group_desc *out); /* fs/ext_common/ext2.c */
int ext2_read_inode(uint32_t number, struct ext2_inode *out); /* fs/ext_common/ext2.c */
int ext2_write_node(const struct storage_node *node, uint64_t offset, const void *buffer,
                    uint32_t length, uint32_t *written); /* fs/ext_common/ext2.c */
int ext2_truncate_file(const struct storage_node *node, uint64_t length); /* fs/ext_common/ext2.c */
int ext2_destroy_inode(uint32_t inode_no, uint8_t directory); /* fs/ext_common/ext2.c */
uint8_t ed_type(uint16_t mode); /* fs/ext_common/dir.c */
extern const unsigned char exfat_standard_upcase[]; /* fs/exfat/exfat_upcase_data.h */
extern const unsigned int exfat_standard_upcase_len; /* fs/exfat/exfat_upcase_data.h */
int fat32_mount(void); /* fs/fat/fat32_mount.c */
int exfat_mount(void); /* fs/exfat/exfat_mount.c */
void exfat_cache_invalidate(void); /**
 * @brief Borrow a held native inode record by volume and backing inode number.
 * @param volume Live native volume slot.
 * @param inode Backing inode or first-cluster number to match.
 * @return Borrowed matching record or NULL; no reference is acquired.
 * @par Context
 * Storage execution ownership required. Keep that ownership through use or acquire an independent inode lease; lookup does not validate a mount generation.
 */
struct storage_inode_ref *storage_inode_find(uint32_t volume, uint32_t inode); /* fs/vfs/inode.c */

#endif /* RELIEFNT_FS_INTERNAL_H */

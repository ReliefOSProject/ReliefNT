#ifndef RELIEFNT_STORAGE_INTERNAL_H
#define RELIEFNT_STORAGE_INTERNAL_H
#include <reliefnt/mm.h>
#include <reliefnt/console.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>
#include <reliefnt/pci.h>
#include <reliefnt/port.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/audio.h>
#include <reliefnt/text_utf16.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/syscall.h>
#include <reliefnt/time.h>
#include <reliefnt/lock.h>
#include <reliefnt/pty.h>
#include <linux/mount.h>

#define ATA_CLASS_MASS_STORAGE 0x01u
#define ATA_SUBCLASS_SATA 0x06u
#define ATA_PROGIF_AHCI 0x01u
#define ATA_SUBCLASS_IDE 0x01u
#define ATA_SUBCLASS_NVM 0x08u
#define ATA_PROGIF_NVME 0x02u

#define IDE_PRIMARY_CMD_DEFAULT 0x1f0u
#define IDE_PRIMARY_CTRL_DEFAULT 0x3f6u
#define IDE_SECONDARY_CMD_DEFAULT 0x170u
#define IDE_SECONDARY_CTRL_DEFAULT 0x376u
#define IDE_STATUS_BSY 0x80u
#define IDE_STATUS_DRDY 0x40u
#define IDE_STATUS_DF 0x20u
#define IDE_STATUS_DRQ 0x08u
#define IDE_STATUS_ERR 0x01u
#define IDE_CMD_READ_PIO 0x20u
#define IDE_CMD_WRITE_PIO 0x30u
#define IDE_CMD_READ_PIO_EXT 0x24u
#define IDE_CMD_WRITE_PIO_EXT 0x34u
#define IDE_CMD_IDENTIFY_PACKET 0xa1u
#define IDE_CMD_SRST 0x04u
#define IDE_CMD_PACKET 0xa0u
#define IDE_ATAPI_SIG_MID 0x14u
#define IDE_ATAPI_SIG_HIGH 0xebu
#define IDE_WAIT_SPINS 4000000u
#define IDE_MAX_PIO_SECTORS 128u
#define IDE_MAX_ATAPI_BLOCKS 16u

#define AHCI_GHC_AE 0x80000000u
#define AHCI_PORT_CMD_ST 0x0001u
#define AHCI_PORT_CMD_FRE 0x0010u
#define AHCI_PORT_CMD_FR 0x4000u
#define AHCI_PORT_CMD_CR 0x8000u
#define AHCI_PORT_TFD_BSY 0x80u
#define AHCI_PORT_TFD_DRQ 0x08u
#define AHCI_PORT_IS_TFES 0x40000000u
#define AHCI_PORT_SIG_ATA 0x00000101u
#define AHCI_PORT_SIG_ATAPI 0xeb140101u
#define AHCI_PORT_DET_PRESENT 0x3u
#define AHCI_PORT_IPM_ACTIVE 0x1u

#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u
#define ATA_CMD_PACKET 0xa0u
#define ATA_CMD_IDENTIFY_DEVICE 0xECu
#define ATA_DEV_BUSY 0x80u
#define ATA_DEV_DRQ 0x08u

#define FIS_TYPE_REG_H2D 0x27u

#define SCSI_CMD_READ10 0x28u

#define AHCI_CMDH_PRDTL 1u
#define AHCI_MAX_SECTORS 64u
#define STORAGE_WRITE_MAX_SECTORS AHCI_MAX_SECTORS
/* AHCI completion is polled synchronously. Keep enough headroom for a busy
 * virtual disk: a too-short limit abandons a live command and turns the next
 * application image read into a spurious -EIO. Filesystem syscalls already
 * bound each transfer, which is the responsiveness control for this path. */
#define AHCI_WAIT_SPINS 40000000u
/* Most virtual AHCI reads complete just after the command is submitted.  In
 * asynchronous syscall context, poll briefly before yielding a whole PIT
 * tick: this keeps application image and UI-resource loading fast without
 * restoring the old unbounded Ring-0 busy wait on a slow disk. */
#define AHCI_ASYNC_FAST_POLL_SPINS 8192u
/* A pending asynchronous command may be polled by transparently resumed
 * syscalls for at most five seconds. A failed virtual controller must not
 * leave a task blocked forever. */
#define AHCI_ASYNC_TIMEOUT_TICKS (5u * 100u)
/* Retrying an individual sector command is safe: all callers resubmit the
 * same LBA range with the same payload.  This contains short-lived virtual
 * AHCI controller faults without replaying a higher-level FAT operation. */
#define AHCI_IO_RETRY_COUNT 3u

#define SECTOR_SIZE 512u
#define GPT_ENTRY_COUNT 128u
#define GPT_ENTRY_SIZE 128u

#define FAT32_EOC 0x0ffffff8u
#define FAT32_ATTR_DIRECTORY 0x10u
#define FAT32_ATTR_VOLUME 0x08u
#define FAT32_ATTR_LFN 0x0fu
#define FAT32_ATTR_HIDDEN 0x02u
#define FAT32_ATTR_SYSTEM 0x04u
#define FAT32_ATTR_ARCHIVE 0x20u
#define FAT32_DIR_ENTRY_BYTES 32u

#define STORAGE_SCRATCH_SECTORS 8u
#define STORAGE_FAT_CACHE_SECTORS 8u
/* Keep the FAT cache fill to a 4 KiB DMA transfer.  This is the proven
 * compatibility limit for resource loads on the current virtual AHCI path;
 * the short completion poll below removes the common full-tick delay without
 * changing the transfer shape used by TTF and BMP loading. */
#define STORAGE_READAHEAD_SECTORS 8u
/* Keep the chain scratch space bounded even for legacy 512-byte-cluster
 * FAT32 volumes. New VMDK images and installer-created volumes use larger
 * clusters, but this remains the safe compatibility limit. */
#define FAT32_MAX_FILE_CLUSTERS 65536u
#define ISO9660_BLOCK_SIZE 2048u
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
#define EXT2_FT_SYMLINK 7u
#define STORAGE_MAX_VOLUMES 10u
#define STORAGE_VOLUME_ROOT 0u
#define STORAGE_VOLUME_TARGET_ROOT 1u
#define STORAGE_VOLUME_BOOT 2u
#define STORAGE_VOLUME_DYNAMIC_FIRST 3u
#define STORAGE_MAX_INSTALL_DISKS RELIEFOS_INSTALL_MAX_DISKS
#define STORAGE_PATH_CACHE_ENTRIES 128u
#define STORAGE_DIR_INDEX_ENTRIES 512u
#define INSTALL_ESP_FIRST_LBA 2048ULL
#define INSTALL_ESP_SECTORS 262144ULL
#define INSTALL_EXT2_BLOCK_SIZE 4096u
#define INSTALL_EXT2_BLOCKS_PER_GROUP 32768u
#define INSTALL_EXT2_INODES_PER_GROUP 8192u
#define EXFAT_EOC 0xffffffffu
#define EXFAT_ENTRY_SIZE 32u
#define EXFAT_ENTRY_FILE 0x85u
#define EXFAT_ENTRY_STREAM 0xc0u
#define EXFAT_ENTRY_NAME 0xc1u
#define EXFAT_ENTRY_BITMAP 0x81u
#define EXFAT_ENTRY_UPCASE 0x82u
#define EXFAT_ATTR_DIRECTORY 0x0010u
#define EXFAT_STREAM_NO_FAT_CHAIN 0x02u

enum storage_volume_kind {
    STORAGE_VOLUME_NONE = 0,
    STORAGE_VOLUME_AHCI = 1,
    STORAGE_VOLUME_RAM = 2,
    STORAGE_VOLUME_IDE = 3,
    STORAGE_VOLUME_NVME = 4,
};

enum storage_transport {
    STORAGE_TRANSPORT_AHCI = 1,
    STORAGE_TRANSPORT_IDE_PIO = 2,
    STORAGE_TRANSPORT_NVME = 3,
};

enum storage_filesystem_kind {
    STORAGE_FILESYSTEM_NONE = 0,
    STORAGE_FILESYSTEM_FAT32 = 1,
    STORAGE_FILESYSTEM_ISO9660 = 2,
    STORAGE_FILESYSTEM_EXT2 = 3,
    STORAGE_FILESYSTEM_EXFAT = 4,
    STORAGE_FILESYSTEM_TMPFS = 5,
    STORAGE_FILESYSTEM_EXT4 = 6,
};

struct __attribute__((packed)) ahci_hba_port {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
};

struct __attribute__((packed)) ahci_hba_mem {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
    uint8_t reserved[0xa0 - 0x2c];
    uint8_t vendor[0x100 - 0xa0];
    struct ahci_hba_port ports[32];
};

struct __attribute__((packed)) ahci_cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct __attribute__((packed)) ahci_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc;
};

struct __attribute__((packed)) ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt[1];
};

struct __attribute__((packed)) fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t reserved0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
};

struct __attribute__((packed)) gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
};

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
int storage_ext4_journal_open(struct storage_volume *);
int storage_ext4_journal_start(struct storage_volume *, uint32_t, struct storage_ext4_handle *);
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
void storage_ext4_cache_invalidate_write(const struct storage_volume *,uint64_t,uint32_t);
int storage_ext4_device_flush(const struct storage_volume *);
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
#define EXT4_INDEX_FL 0x1000u
int storage_ext4_dir_hash(struct storage_volume *,const char *,unsigned,uint32_t *);
struct storage_ext4_dirent { uint64_t ino; uint8_t type, name_len; char name[256]; };
int storage_ext4_dir_iterate(struct storage_volume *,uint64_t,uint64_t *,struct storage_ext4_dirent *);
int storage_ext4_dir_lookup(struct storage_volume *,uint64_t,const char *,uint64_t *,uint8_t *);
int storage_ext4_dir_insert(struct storage_volume *,uint64_t,const char *,uint64_t,uint8_t);
int storage_ext4_dir_remove(struct storage_volume *,uint64_t,const char *,uint64_t *);
int storage_ext4_create(struct storage_volume *,uint64_t,const char *,uint16_t,uint32_t,uint32_t,const char *,uint64_t *);
int storage_ext4_link(struct storage_volume *,uint64_t,const char *,uint64_t);
int storage_ext4_unlink(struct storage_volume *,uint64_t,const char *,bool);
int storage_ext4_rename(struct storage_volume *,uint64_t,const char *,uint64_t,const char *);
int storage_ext4_orphan_add(struct storage_volume *,uint64_t,struct ext4_inode_view *);
int storage_ext4_unlink_held(struct storage_volume *,uint64_t,const char *,bool,bool);
int storage_ext4_rename_held(struct storage_volume *,uint64_t,const char *,uint64_t,const char *,bool);
bool storage_ext4_is_ext_family(const struct storage_volume *);
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

/* ---- ext4 mount and feature policy (task 4) --------------------------
 * storage_ext4_mount() parses the whole ext2/3/4 family with the unified
 * parser above.  It returns 0 and fills volume->ext4 / volume->filesystem /
 * volume->read_only_reason on success, or a negative RELIEFOS_* errno:
 * -RELIEFOS_EINVAL for format or partition-boundary violations,
 * -RELIEFOS_EIO for superblock / group-descriptor / root-inode checksum
 * failures and corrupt metadata, -RELIEFOS_EOPNOTSUPP when the feature
 * policy rejects the image (the offending bit mask is logged; the volume
 * keeps its previous contents).  Failed mounts release the per-volume ext4
 * state and cache.  volume->mount_generation increments on every mount
 * attempt; cache entries are stamped with it so a reused volume object never
 * serves blocks from a previous mount.
 *
 * storage_ext4_feature_policy() maps the feature words to a verdict:
 * RW or RO with a STORAGE_EXT4_READ_ONLY_* reason (returns 0), or REJECT
 * (returns -RELIEFOS_EOPNOTSUPP).  Read-only reasons compose with the fixed
 * precedence READONLY_FEATURE > UNKNOWN_RO_COMPAT > JOURNAL_NEEDS_RECOVERY,
 * so clearing the transient RECOVER flag (journal replay, a later task) can
 * restore write access only when no permanent reason was present. */

/* Defined further below; declared here so the prototypes do not give the
 * tag prototype scope. */
struct storage_volume;

#define STORAGE_EXT4_READ_ONLY_NONE 0u
#define STORAGE_EXT4_READ_ONLY_READONLY_FEATURE 1u
#define STORAGE_EXT4_READ_ONLY_UNKNOWN_RO_COMPAT 2u
#define STORAGE_EXT4_READ_ONLY_JOURNAL_NEEDS_RECOVERY 3u
#define STORAGE_EXT4_READ_ONLY_JOURNAL_CORRUPT 4u

enum storage_ext4_feature_verdict {
    STORAGE_EXT4_FEATURE_RW = 0,
    STORAGE_EXT4_FEATURE_RO = 1,
    STORAGE_EXT4_FEATURE_REJECT = 2,
};

struct storage_ext4_feature_decision {
    uint8_t verdict;
    uint32_t read_only_reason;
};

int storage_ext4_feature_policy(const struct storage_ext4_super_view *view,
                                struct storage_ext4_feature_decision *out);
int storage_ext4_mount(struct storage_volume *volume);
/* Drops the volume's ext4 cache entries and clears ext4 state,
 * read_only_reason and the filesystem kind (mount failure and ext2
 * fallback cleanup). */
void storage_ext4_state_reset(struct storage_volume *volume);

/* ---- ext4 bounded caches (storage_ext4_cache.c) ----------------------
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
void storage_ext4_cache_invalidate(struct storage_volume *volume);

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

/* ---- ext4 metadata checksums ----------------------------------------
 * Linux-compatible checksum layer (linux/fs/ext4/{super.c,inode.c} and
 * linux/lib/crc/crc16.c, Linux v7.3-rc5 reference tree).  Pure functions
 * over caller-provided buffers: verify() never writes, update() stamps the
 * checksum field (little-endian) of the buffer it was given and nothing
 * else.  The verify/update entry points return 0 on match/write, 0 without
 * touching the buffer when the relevant checksum feature is off (super and
 * inode: METADATA_CSUM; group descriptor: METADATA_CSUM or GDT_CSUM),
 * -RELIEFOS_EIO on a checksum mismatch, and -RELIEFOS_EINVAL for NULL
 * pointers or out-of-range sizes (superblock below EXT4_SUPERBLOCK_SIZE,
 * desc_size outside 32..256 or not a multiple of 4, inode_size outside
 * 128..EXT4_MAX_BLOCK_SIZE or not a multiple of 128).
 *
 * inode checksums additionally presuppose s_creator_os == EXT4_OS_LINUX(0)
 * as in Linux; that field is not part of the super view, so callers must
 * not checksum inodes of non-Linux-created filesystems.  Whether the
 * i_checksum_hi slot at 0x82 takes part follows EXT4_FITS_IN_INODE
 * (i_extra_isize >= 4): where it does not fit, 0x82 onward is ordinary
 * hash input and only the low 16 bits at 0x7C are stored or compared.
 * The legacy gdt_csum chain only hashes the descriptor tail past
 * bg_checksum when INCOMPAT_64BIT is set (super.c:3293-3295). */
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

/* ---- ext4 group descriptors, bitmaps and the flex-group allocator
 * (storage_ext4_alloc.c, task 5) ------------------------------------------
 * All six entry points return 0 on success or a negative RELIEFOS_* errno:
 * -RELIEFOS_EINVAL for NULL pointers, zero counts, out-of-range groups /
 * blocks / inodes and double frees (freeing a block or inode whose bitmap
 * bit is clear), -RELIEFOS_EIO for I/O or checksum failures (the volume's
 * ext4.fs_error is set and logged when a bitmap checksum mismatches), and
 * -RELIEFOS_ENOSPC when no block/inode at all is available.  All block
 * numbers are uint64_t throughout.
 *
 * storage_ext4_read_group() serves a group descriptor from the bounded
 * group-summary cache (EXT4_GROUP_CACHE_ENTRIES entries, LRU, stamped with
 * the volume's mount_generation); on a miss it reads the descriptor
 * through the block cache (s_first_data_block+1 up, desc_size wide,
 * straddling block boundaries when needed) and verifies its checksum.
 * storage_ext4_write_group() writes the whole view back (bitmap/table
 * addresses, free counts, used_dirs, itable_unused, flags and the two
 * bitmap checksum words; bytes the view does not carry are preserved),
 * recomputes bg_checksum and refreshes the summary cache.  Bitmap,
 * descriptor and superblock free-count updates are published through
 * storage_ext4_cache_mark_dirty(); journal handles wrap these updates in a
 * later task.
 *
 * storage_ext4_alloc_blocks() searches near goal inside goal's group first
 * (forward and backward), then the rest of goal's flex group, then the
 * other flex groups ordered by their accumulated free-block counts
 * (flex groups whose free count can not satisfy count are tried last).
 * Each group's bitmap is searched at most once; a run may cross a group
 * boundary when scanning near goal (contiguous block numbers).  When no
 * full run of count blocks exists the best (longest) shorter run is
 * allocated and *allocated reports its length; with no free block at all
 * the call returns -RELIEFOS_ENOSPC.  Success moves the per-volume
 * next-goal hint and its reservation window of
 * EXT4_RESERVATION_WINDOW_BLOCKS blocks: blocks inside the window are not
 * handed to requests whose goal lies outside it (the window moves with
 * every successful allocation and is released by any successful
 * storage_ext4_free_blocks(), so recycling never hides blocks).  goal must
 * be < blocks_count (else -EINVAL); goals below s_first_data_block are
 * clamped up to it.
 *
 * BLOCK_UNINIT (uninit_bg) groups are treated as all-free but uninitialized
 * and are initialized on first touch: in-group metadata (superblock
 * backup, group descriptor table, the bitmap blocks themselves and the
 * inode table) and blocks past the end of the filesystem get their bits
 * set, the free-block count is recomputed from the bitmap, the flag is
 * cleared and the bitmap plus descriptor are written back (with fresh
 * checksums).  INODE_UNINIT works the same way for the inode bitmap (tail
 * bits past s_inodes_count and the bitmap padding are set, bg_itable_unused
 * is set to s_inodes_per_group); INODE_UNINIT on group 0 is corruption
 * (-RELIEFOS_EIO, matching Linux).
 *
 * storage_ext4_alloc_inode() reserves inodes 1..10 (EXT4_GOOD_OLD_FIRST_INO
 * and below are never returned).  Regular inodes take the first group with
 * free inodes; directories prefer the group with the most free inodes.
 * storage_ext4_free_inode() validates the range, that the bitmap bit is
 * set and that the on-disk inode's i_links_count is 0, zeroes the inode
 * table slot through the cache and decrements bg_used_dirs_count for
 * directories. */
#define EXT4_GOOD_OLD_FIRST_INO 11u
/* Bounded group-summary cache size; overridable with -D like the block
 * cache capacities (host tests shrink it to exercise eviction). */
#ifndef EXT4_GROUP_CACHE_ENTRIES
#define EXT4_GROUP_CACHE_ENTRIES 64u
#endif
/* Blocks reserved after a successful allocation for the same writer. */
#ifndef EXT4_RESERVATION_WINDOW_BLOCKS
#define EXT4_RESERVATION_WINDOW_BLOCKS 8u
#endif
/* s_log_groups_per_flex clamp: flex groups above 2^7 members make the
 * on-demand free-count sums expensive for no benefit (mount logs when it
 * applies the clamp). */
#define EXT4_MAX_FLEX_LOG 7u

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

struct nvme_controller;

static const uint8_t esp_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

static const uint8_t linux_filesystem_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

/* Microsoft Basic Data GUID in GPT's little-endian on-disk byte order. */
static const uint8_t basic_data_guid[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

struct storage_volume {
    bool ready;
    struct tmpfs_super *tmpfs;
    char tmpfs_source[RELIEFOS_FS_PATH_LEN];
    uint64_t mount_flags;
    uint8_t volume_id;
    uint8_t kind;
    uint8_t filesystem;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t port;
    uint8_t transport;
    uint8_t ide_channel;
    uint8_t ide_drive;
    uint8_t ide_atapi;
    uint16_t ide_command_base;
    uint16_t ide_control_base;
    uint8_t ide_lba48;
    char device_model[41];
    struct ahci_hba_mem *abar;
    struct ahci_hba_port *hba_port;
    struct nvme_controller *nvme;
    uint16_t nvme_controller_index;
    uint32_t nvme_nsid;
    uint8_t *ram_base;
    uint64_t ram_bytes;
    uint64_t esp_start_lba;
    uint64_t esp_sector_count;
    /* Ext-family partition range (ext2/3/4 share one parser and one GPT
     * partition); the legacy ext2_* spellings live on as access macros. */
    uint64_t ext_start_lba;
    uint64_t ext_sector_count;
    uint64_t exfat_start_lba;
    uint64_t exfat_sector_count;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_count;
    uint32_t fat_start_sector;
    uint32_t fat_sector_count;
    uint32_t data_start_sector;
    uint32_t total_sectors;
    uint32_t data_cluster_count;
    uint32_t root_cluster;
    uint16_t fat_fs_info_sector;
    uint16_t fat_backup_boot_sector;
    uint32_t fat_free_clusters;
    uint8_t fat_fsinfo_valid;
    uint32_t ext2_block_size;
    uint32_t ext2_blocks_count;
    uint32_t ext2_blocks_per_group;
    uint32_t ext2_inodes_per_group;
    uint32_t ext2_inode_size;
    uint32_t ext2_first_data_block;
    uint32_t ext2_group_count;
    uint32_t ext2_feature_incompat;
    uint32_t ext2_next_block;
    uint32_t ext2_next_inode;
    /* ext4 backend state (geometry parsed at mount, bounded caches held by
     * storage_ext4_cache.c) plus mount bookkeeping: mount_generation is
     * stamped into cache entries and read_only_reason is one of the
     * STORAGE_EXT4_READ_ONLY_* values (0 = writable). */
    struct storage_ext4_state ext4;
    uint32_t mount_generation;
    uint32_t read_only_reason;
    uint32_t exfat_fat_offset;
    uint32_t exfat_fat_length;
    uint32_t exfat_cluster_heap_offset;
    uint32_t exfat_cluster_count;
    uint32_t exfat_root_cluster;
    uint32_t exfat_bitmap_cluster;
    uint32_t exfat_upcase_cluster;
    uint64_t exfat_bitmap_length;
    uint64_t exfat_upcase_length;
    uint32_t exfat_upcase_checksum;
    uint32_t exfat_next_free_cluster;
    uint8_t exfat_sectors_per_cluster_shift;
    uint8_t exfat_bitmap_nofat;
    uint8_t exfat_upcase_nofat;
    uint32_t iso_block_size;
    uint32_t iso_root_extent;
    uint32_t iso_root_size;
    uint64_t iso_sector_count;
    uint32_t next_free_cluster;
    uint8_t gpt_disk_guid[16];
    uint8_t esp_unique_guid[16];
    uint8_t ext2_unique_guid[16];
    uint8_t exfat_unique_guid[16];
    uint64_t esp_device;
    uint64_t ext_device;
    uint64_t exfat_device;
    uint8_t has_gpt_identity;
    uint32_t source_disk_id;
    uint32_t source_partition_index;
    uint8_t data_partition_mount;
    char mount_path[RELIEFOS_FS_PATH_LEN];
};

/* Spec section 8: the ext2 partition range was renamed to the ext-family
 * spelling.  Keep the old field names working as access macros so existing
 * callers and host-test fixtures compile unchanged. */
#define ext2_start_lba ext_start_lba
#define ext2_sector_count ext_sector_count

/* Host tests compile the ext4 storage fragments as standalone translation
 * units (task 5 links storage_ext4_{format,checksum,cache,alloc}.c without
 * the storage.c facade) and provide their own storage_read_device /
 * storage_write_device / storage_memzero / storage_memcpy.  The facade
 * defines these four helpers as static in earlier fragments, so plain
 * prototypes here would break that TU ("static declaration follows
 * non-static declaration"); standalone builds opt in with
 * -DRELIEFOS_STORAGE_STANDALONE_TU to get the prototypes. */
#ifdef RELIEFOS_STORAGE_STANDALONE_TU
void storage_memzero(void *dst, size_t len);
void storage_memcpy(void *dst, const void *src, size_t len);
int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                        uint32_t sector_count, void *buffer);
int storage_write_device(const struct storage_volume *volume, uint64_t lba,
                         uint32_t sector_count, const void *buffer);
#endif

struct install_disk_state {
    bool present;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t port;
    uint8_t transport;
    uint8_t ide_channel;
    uint8_t ide_drive;
    uint8_t ide_atapi;
    uint8_t ide_lba48;
    uint16_t ide_command_base;
    uint16_t ide_control_base;
    char device_model[41];
    uint8_t boot_root;
    uint8_t target_mounted;
    struct ahci_hba_mem *abar;
    struct ahci_hba_port *hba_port;
    struct nvme_controller *nvme;
    uint16_t nvme_controller_index;
    uint32_t nvme_nsid;
    uint64_t sector_count;
};

/* Public block-device spelling used by devfs, mount tables, and GPT links.
 * The storage core continues to address disks by their probe-time disk_id. */
int storage_disk_device_name(uint32_t disk_id, int32_t partition_index,
                             char *out, uint32_t capacity);
int storage_parse_block_name(const char *name, uint32_t *disk_id,
                             int32_t *partition_index);

static struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
static struct storage_volume *g_active_volume = &g_volumes[0];
#define g_storage (*g_active_volume)

/*
 * A filesystem syscall is allowed to park while it is only observing disk
 * state.  Once it has begun a mutation, however, replaying the syscall after
 * an asynchronous yield could allocate a second FAT chain or apply part of a
 * directory update twice.  Keep that small critical tail synchronous; normal
 * application loads, path walks and compiler header reads remain preemptible.
 */
static bool storage_io_async_context;
static bool storage_io_write_started;
/* The AHCI command table, DMA staging areas and FAT32 caches are shared
 * globally.  A syscall that yielded for disk completion therefore owns the
 * storage state until its instruction is retried and completes. */
static uint32_t storage_task_io_owner;

static struct install_disk_state g_install_disks[STORAGE_MAX_INSTALL_DISKS];
static uint32_t g_install_disk_count;
static uint8_t g_devfs_enabled = 1;
static uint8_t g_installer_root_active;

/* AHCI uses shared command RAM and IDE PIO owns channel registers while an
 * operation is in flight.  NVMe has a queue per controller, but it shares a
 * staging page with the transport.  Serialize these synchronous operations
 * without disabling interrupts, so SMP callers cannot corrupt a command. */
static struct kernel_spinlock storage_transport_lock = KERNEL_SPINLOCK_INIT;

static uint8_t ahci_received_fis[256] __attribute__((aligned(256)));
static uint8_t ahci_cmd_table_buf[256] __attribute__((aligned(128)));
static struct ahci_cmd_header ahci_cmd_headers[32] __attribute__((aligned(1024)));
static uint8_t storage_scratch[STORAGE_SCRATCH_SECTORS * SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t storage_cluster_buf[64 * SECTOR_SIZE] __attribute__((aligned(4096)));

/* The public /dev block namespace is rebuilt whenever storage is probed. */
void storage_disk_block_cache_reset(void);
static uint8_t storage_fat_cache_data[STORAGE_FAT_CACHE_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
static uint8_t storage_read_cache_data[STORAGE_READAHEAD_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
/* Directory lookups are much more frequent than file-data reads during a
 * compiler build. Keep a separate cluster cache so opening the next header
 * does not evict the directory that is being scanned from the data cache. */
static uint8_t storage_dir_lookup_cache_data[64 * SECTOR_SIZE]
    __attribute__((aligned(4096)));
static uint32_t storage_old_chain[FAT32_MAX_FILE_CLUSTERS];
static uint32_t storage_new_chain[FAT32_MAX_FILE_CLUSTERS];

struct ahci_pending_command {
    struct ahci_hba_port *port;
    uint64_t lba;
    uint32_t sector_count;
    void *buffer;
    uint64_t start_tick;
    uint32_t owner_pid;
    uint8_t write;
    uint8_t active;
};

static struct ahci_pending_command ahci_pending_command;

/* Defined by storage_mount.c after transport discovery helpers.  NVMe's
 * namespace enumerator calls this to give root selection identical behavior
 * to AHCI and IDE disks. */
static int storage_try_mount_root_disk(struct install_disk_state *disk);

/* Implemented by storage_disk.c, which is included after this module. */
int storage_disk_block_info(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count);

struct storage_sector_cache {
    struct storage_volume *volume;
    uint64_t first_lba;
    uint32_t sector_count;
    uint8_t valid;
};

struct storage_cluster_cache {
    struct storage_volume *volume;
    uint32_t cluster;
    uint8_t valid;
};

struct storage_path_cache_entry {
    struct storage_volume *volume;
    struct storage_node node;
    char path[RELIEFOS_FS_PATH_LEN];
    uint8_t valid;
};

struct storage_dir_index_entry {
    struct storage_volume *volume;
    uint32_t directory_cluster;
    struct storage_node node;
    char name[RELIEFOS_FS_NAME_LEN];
    uint8_t valid;
};

static struct storage_sector_cache storage_fat_cache;
static struct storage_sector_cache storage_read_cache;
static struct storage_cluster_cache storage_dir_lookup_cache;
static struct storage_path_cache_entry storage_path_cache[STORAGE_PATH_CACHE_ENTRIES];
static uint32_t storage_path_cache_next;
static struct storage_dir_index_entry storage_dir_index[STORAGE_DIR_INDEX_ENTRIES];
static uint32_t storage_dir_index_next;

/* Keep the last file chain tail available for repeated append writes. The
 * scheduler limits each syscall to a small payload, so without this hint a
 * large file would be walked from its first cluster on every syscall. */
struct storage_write_chain_cache {
    struct storage_volume *volume;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t count;
    uint32_t tail;
    char path[RELIEFOS_FS_PATH_LEN];
    uint8_t valid;
};

static struct storage_write_chain_cache storage_write_chain_cache;

/* Directory reads are issued one entry at a time. Cache the current cluster
 * and decoded entry ordinal so sequential readdir never restarts at root. */
struct storage_dir_iter_cache {
    struct storage_volume *volume;
    uint32_t first_cluster;
    uint32_t cluster;
    uint32_t entry_offset;
    uint64_t next_index;
    uint8_t valid;
};

static struct storage_dir_iter_cache storage_dir_iter_cache;

static int fat32_mount(void);
static int exfat_mount(void);
static void exfat_cache_invalidate(void);
static void storage_put_u32(uint8_t *p, uint32_t value);

static uint32_t storage_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* RELIEFNT_STORAGE_INTERNAL_H */

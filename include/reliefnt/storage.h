/*
 * ReliefOS storage interface: declares filesystem and storage-node operations.
 * Provides path lookup, directory access, file reads, and mount abstractions.
 */
#ifndef RELIEFNT_STORAGE_H
#define RELIEFNT_STORAGE_H

#include <reliefos/fs_abi.h>
#include <reliefos/system_abi.h>
#include <reliefnt/types.h>
#include <linux/stat.h>
#include <linux/statfs.h>

/**
 * @brief Internal POSIX permission triple carried between the storage
 *        backends, the ACL sidecar and the permission checker.
 *
 * Ring-3 code never sees this layout: userland reads and writes POSIX
 * permissions through the Linux stat/chmod/chown ABI, which is translated at
 * the syscall boundary.  Keeping it here stops an internal representation from
 * being frozen as a published ABI.
 */
struct reliefos_permissions {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};
/** @brief Assemble the initial shared namespace from explicit boot roles.
 * @return Zero or negative errno. Called once after root/controller probing,
 * before kernel configuration readers, runtime-directory preparation or PID1,
 * under exclusive execution ownership with synchronous storage. Publishes
 * root/devfs/proc/sysfs, the ready boot ESP, numbered optical instances and a
 * fresh sticky shm filesystem together, without consulting volume path labels.
 * Failure retires unpublished mounts and this call's new shm context; existing
 * backends are never destroyed. No graph/module lock spans provider callbacks.
 */
int storage_namespace_bootstrap(void);

/* Legacy installation records are internal-only while boot storage is being
 * simplified.  They are deliberately absent from the public SDK; userland
 * reaches disks exclusively through /dev and Linux-compatible interfaces. */
#define RELIEFOS_INSTALL_MAX_DISKS 8U
#define RELIEFOS_INSTALL_DISK_FLAG_BOOT_ROOT 0x00000001U
#define RELIEFOS_INSTALL_DISK_FLAG_TARGET_MOUNTED 0x00000002U
#define RELIEFOS_DISK_MAX_PARTITIONS 128U
#define RELIEFOS_DISK_PARTITION_NAME_LEN 72U
#define RELIEFOS_DISK_PARTITION_TYPE_BASIC_DATA 1U
#define RELIEFOS_DISK_PARTITION_TYPE_ESP 2U
#define RELIEFOS_DISK_PARTITION_TYPE_LINUX 3U
#define RELIEFOS_DISK_PARTITION_EDIT_TYPE 0x00000001U
#define RELIEFOS_DISK_PARTITION_EDIT_NAME 0x00000002U
#define RELIEFOS_DISK_FILESYSTEM_UNKNOWN 0U
#define RELIEFOS_DISK_FILESYSTEM_FAT32 1U
#define RELIEFOS_DISK_FILESYSTEM_EXT2 2U
#define RELIEFOS_DISK_FILESYSTEM_ISO9660 3U
#define RELIEFOS_DISK_FILESYSTEM_EXFAT 4U
#define RELIEFOS_DISK_FILESYSTEM_EXT4 5U
#define RELIEFOS_DISK_PARTITION_FLAG_ESP 0x00000001U
#define RELIEFOS_DISK_PARTITION_FLAG_BOOT_ROOT 0x00000002U
#define RELIEFOS_DISK_PARTITION_FLAG_TARGET_MOUNTED 0x00000004U
#define RELIEFOS_DISK_PARTITION_FLAG_PROTECTED 0x00000008U
#define RELIEFOS_DISK_PARTITION_FLAG_MOUNTED 0x00000010U
#define RELIEFOS_DISK_GPT_INITIALIZE_FORCE 0x00000001U

struct reliefos_install_disk {
    uint32_t id, port, sector_size, flags;
    uint64_t sector_count;
    char name[32];
};
struct reliefos_disk_partition {
    uint32_t disk_id, index, filesystem, flags;
    char mount_path[RELIEFOS_FS_PATH_LEN];
    uint64_t first_lba, sector_count;
    uint8_t type_guid[16];
    char name[RELIEFOS_DISK_PARTITION_NAME_LEN];
};
struct reliefos_disk_partition_format { uint32_t disk_id, partition_index, filesystem, reserved; };
struct reliefos_disk_partition_delete { uint32_t disk_id, partition_index, reserved0, reserved1; };
struct reliefos_disk_partition_create { uint32_t disk_id, filesystem, size_mib, reserved; char name[RELIEFOS_DISK_PARTITION_NAME_LEN]; };
struct reliefos_disk_partition_mount { uint32_t disk_id, partition_index; char mount_path[RELIEFOS_FS_PATH_LEN]; };
struct reliefos_disk_partition_unmount { uint32_t disk_id, partition_index, reserved0, reserved1; };
struct reliefos_disk_partition_edit { uint32_t disk_id, partition_index, edit_mask, type, reserved; char name[RELIEFOS_DISK_PARTITION_NAME_LEN]; };
struct reliefos_disk_gpt_initialize { uint32_t disk_id, flags; };

struct storage_node {
    uint32_t type;
    uint32_t flags;
    uint32_t first_cluster;
    uint32_t volume_id;
    uint64_t size;
    uint32_t mount_generation;
    uint32_t inode_generation;
    /* Backend-native directory-record/key identity, never a userspace inode ABI.
     * FAT/exFAT use an on-disk record location; anonymous shmem uses a held,
     * generation-checked kernel object handle. */
    uint64_t entry_id;
};

struct storage_inode_ref;
struct vfs_mount;
/** @brief Create an owned detached runtime tmpfs attachment with final policy.
 * @param source Optional kernel diagnostic source label, copied with PATH_MAX bounds.
 * @param flags Final supported mount flags, without operation bits.
 * @param options Borrowed NUL-terminated tmpfs option string, nullable.
 * @param uid Root inode owner filesystem UID.
 * @param gid Root inode owner filesystem GID.
 * @param out Receives one owned detached mount; empty on every failure.
 * @return Zero or negative option/quota/allocation/capacity errno. No target
 * pathname is stored. Final path/mount put destroys the owned backend after
 * all inode pins retire. Caller holds execution ownership, no graph locks.
 */
int storage_new_tmpfs_mount(const char *source,uint64_t flags,const char *options,
                               uint32_t uid,uint32_t gid,struct vfs_mount **out);
/** @brief Open a GPT-backed filesystem as an owned detached graph attachment.
 * @param disk Block disk ID derived from a retained standard device node.
 * @param partition GPT entry index on that disk.
 * @param filesystem Kernel filesystem type, nullable for native detection.
 * @param flags Supported final mount policy, excluding operation bits.
 * @param out Owned detached mount output, cleared on failure.
 * @return Zero or negative device/filesystem/busy/I/O/allocation errno.
 * Mount ownership releases the backend after final reference retirement;
 * no target pathname or prefix alias is installed. Serialized task context,
 * synchronous metadata I/O without graph/module spinlocks.
 */
int storage_new_block_mount(uint32_t disk,uint32_t partition,const char *filesystem,
                               uint64_t flags,struct vfs_mount **out);
/** @brief Acquire an actual inode lease and its resident cluster timestamps.
 * @param node Required native identity; a cluster generation must remain current.
 * @param out Required owned reference output, cleared before validation.
 * @return Zero, including NULL for unsupported synthetic nodes, or negative
 * allocation/stale/native metadata/I/O errno. Serializes under execution
 * ownership. Cluster metadata is captured before unlink can reuse the record;
 * each returned reference requires storage_inode_put. No module/graph callback
 * lock spans native I/O; shared inode/cache ownership outlives directory names.
 */
int storage_inode_get(const struct storage_node *node, struct storage_inode_ref **out);
void storage_inode_retain(struct storage_inode_ref *reference);
int storage_inode_put(struct storage_inode_ref *reference);
int storage_inode_refresh(struct storage_node *node);
/** @brief Acquire one writable-file lease on a held regular inode.
 * @param reference Caller-owned inode pin, required; not transferred.
 * @return Zero, -ETXTBSY while kernel reads deny writes, or negative errno.
 * Task context; serializes through the reentrant execution transaction.
 */
int storage_inode_write_get(struct storage_inode_ref *reference);
/** @brief Release one writable-file lease before dropping its inode pin.
 * @param reference Held inode with one acquired write lease, or NULL.
 * Task context; serializes with deny/acquire through execution ownership.
 */
void storage_inode_write_put(struct storage_inode_ref *reference);
/** @brief Prevent new writers while taking a stable kernel file snapshot.
 * @param reference Caller-owned regular inode pin, required; not transferred.
 * @return Zero, -ETXTBSY if any writable OFD/VMA remains, or negative errno.
 * Multiple kernel guards may coexist. Serialized task context, no backend I/O.
 */
int storage_inode_deny_write(struct storage_inode_ref *reference);
/** @brief Release one kernel read guard before releasing its owned inode pin.
 * @param reference Held inode with an acquired deny guard, or NULL.
 * Task context under the reentrant execution transaction; no backend I/O.
 */
void storage_inode_allow_write(struct storage_inode_ref *reference);
struct storage_ext4_fiemap_extent { uint64_t logical,physical,length; uint32_t flags; };
#define EXT4_FIEMAP_UNWRITTEN 1u
#define EXT4_FIEMAP_HOLE 2u
#define EXT4_FIEMAP_LAST 4u
/** @brief Change ext inode allocation through its retained native provider.
 * @param node Borrowed mutable retained ext inode. @param mode Linux fallocate mode.
 * @param offset Range start. @param length Range length.
 * @return Zero or unsupported/provider errno. Caller authorizes; recursively
 * enters execution and restores selection, no graph/module lock across I/O.
 */
int storage_fallocate_node(struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length);
int storage_fiemap_node(const struct storage_node *,uint64_t,uint64_t,
                       struct storage_ext4_fiemap_extent *,uint32_t,uint32_t *);
int storage_node_mount_flags(const struct storage_node *node, uint64_t *flags);
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys);
/** @brief Reserve pages in the exact selected mounted or anonymous tmpfs inode.
 * @param node Borrowed live node, refreshed on success; not a displayed name.
 * @param mode Zero or Linux KEEP_SIZE; other modes return EOPNOTSUPP.
 * @param offset Nonnegative start byte.
 * @param length Positive byte length with a signed-off_t representable end.
 * @return Zero or allocation/quota/type/range errno with balanced volume
 * selection and execution ownership. Caller retains an inode/path pin.
 */
int storage_tmpfs_fallocate(struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length);
/* Internal shmem filesystem identity; consumes no mounted-volume slot. */
#define STORAGE_SHMEM_VOLUME 0xffu
/**
 * @brief Create an unlinked file on the internal shmem filesystem.
 * @param uid Owner UID of the new file.
 * @param gid Owner GID of the new file.
 * @param node Required output receiving the backing node.
 * @param reference Required output receiving one owned inode reference.
 * @return Zero on success or negative errno. Acquires the execution lock;
 * the caller must eventually release the returned inode reference.
 */
int storage_shmem_create(uint32_t uid, uint32_t gid, struct storage_node *node,
                          struct storage_inode_ref **reference);
/** @brief Settle native filesystem writeback before its registered device flush.
 * @param volume_id Existing native volume slot.
 * @return Zero or first writeback/transport errno; absent/not-ready is ENODEV.
 * Acquires execution exclusion, saves/restores async state and pins provider
 * code through callbacks without holding any filesystem/device registry lock.
 */
int storage_sync_volume(uint32_t volume_id);
/** @brief Sync every ready volume while retaining the first observed error.
 * @return Zero or first native writeback/transport errno; later instances are
 * still attempted. Acquires execution exclusion, delegates per-volume ordering.
 */
int storage_sync_all(void);
int storage_sync_disk(uint32_t disk_id);
int storage_write_held_node(struct storage_node *node, uint64_t offset,
                            const void *buffer, uint32_t length, uint32_t *written);
int storage_truncate_held_node(struct storage_node *node, uint64_t length);

int storage_inode_permissions(const struct storage_node *node,
                              struct reliefos_permissions *value, bool write);
int storage_inode_stat(const struct storage_node *node, struct linux_stat_abi *value);
int storage_inode_utimensat(const struct storage_node *node, int64_t atime, int64_t mtime,
                            bool set_atime, bool set_mtime);
int storage_statfs(const struct storage_node *node, struct linux_statfs_abi *value);

/**
 * @brief Maintains the next FAT32 cluster for one sequential file reader.
 *
 * The cursor is deliberately owned by the open file descriptor.  It is not
 * part of a storage node because several descriptors may read the same node
 * at different offsets.
 */
struct storage_read_cursor {
    uint64_t offset;
    uint32_t cluster;
    uint32_t valid;
};

#define STORAGE_NODE_FLAG_ROOT    0x00000001u
#define STORAGE_NODE_FLAG_DEV_DIR 0x00000002u
#define STORAGE_NODE_FLAG_DEV_FB0 0x00000004u
#define STORAGE_NODE_FLAG_EXT2    0x00000008u
#define STORAGE_NODE_FLAG_EXT_FAMILY STORAGE_NODE_FLAG_EXT2
#define STORAGE_NODE_FLAG_EXFAT   0x00000010u
#define STORAGE_NODE_FLAG_EXFAT_NOFAT 0x00000020u
#define STORAGE_NODE_FLAG_DEV_NODE 0x00000040u
#define STORAGE_NODE_FLAG_DEV_BLOCK 0x00000080u
#define STORAGE_NODE_FLAG_PROC    0x00000100u
#define STORAGE_NODE_FLAG_SYSFS 0x00000400u
#define STORAGE_NODE_FLAG_PTY   0x00000800u
#define STORAGE_NODE_FLAG_TMPFS 0x00001000u
#define STORAGE_NODE_FLAG_AUDIO_PCM 0x00002000u
#define STORAGE_NODE_FLAG_AUDIO_CONTROL 0x00004000u
#define STORAGE_NODE_FLAG_AUDIO_MIXER 0x00008000u
#define STORAGE_NODE_FLAG_AUDIO_TIMER 0x00010000u
#define STORAGE_NODE_FLAG_FAT32   0x00020000u
#define STORAGE_NODE_FLAG_MAGIC_LINK 0x00040000u
#define STORAGE_NODE_FLAG_MEMFD 0x00080000u
/* Native inode carrying st_rdev; first_cluster remains its inode identity. */
#define STORAGE_NODE_FLAG_RDEV 0x00100000u
#define STORAGE_SYSFS_DEVICE 202u
#define STORAGE_NODE_FLAG_DEV_LINK 0x00000200u
/* Anonymous filesystem device numbers, also exported in proc mountinfo. */
#define STORAGE_DEVFS_DEVICE 200u
#define STORAGE_PROCFS_DEVICE 201u

/**
 * @brief Read the Linux device identity of a mounted filesystem under the mount lock.
 * @param volume_id Mounted-volume slot, distinct from a block device's encoded ID.
 * @return Linux dev_t matching mountinfo and stat, or zero for an unavailable slot.
 */
uint64_t storage_volume_device(uint32_t volume_id);

/* Device-node volume_id encoding for block devices.  The low 16 bits select
 * the physical disk; the high 16 bits contain GPT entry + 1, or zero for the
 * whole disk.  Device nodes never use volume_id for mounted-volume lookup. */
#define STORAGE_BLOCK_DISK_ID(value) ((uint32_t)(value) & 0xffffu)
#define STORAGE_BLOCK_PARTITION(value) ((int32_t)(((uint32_t)(value) >> 16) & 0xffffu) - 1)
#define STORAGE_BLOCK_MAJOR 259u
#define STORAGE_BLOCK_MINOR(value) (STORAGE_BLOCK_DISK_ID(value) * 256u + STORAGE_BLOCK_PARTITION(value) + 1u)
struct block_device_ref;
#define STORAGE_BLOCK_OPTICAL_BIT 0x8000u
/** @brief Resolve the immutable Linux dev_t retained by an inventory/node identity.
 * @param value Encoded stable disk/partition ID, including optical major-11 IDs.
 * @return Linux dev_t, or zero for an unknown identity. Execution ownership;
 * scalar observation only, no provider callback/I/O/reference acquisition.
 */
uint64_t storage_block_rdev(uint32_t value);
/** @brief Pin one exact inventory/optical identity for an OFD or full transaction.
 * @param disk_id Stable native disk ID, or flagged optical minor.
 * @param out Empty required owned block-reference output.
 * @return Zero or -ENODEV/lookup errno. Execution ownership required; refreshes
 * scalar inventory without callbacks, and never aliases an unloaded/reloaded ID.
 * Caller releases the token on every close/error after all multi-I/O work.
 */
int storage_disk_block_get(uint32_t disk_id,struct block_device_ref *out);
#define STORAGE_BLOCK_VOLUME_ID(disk, partition) \
    ((uint32_t)(disk) & 0xffffu) | ((uint32_t)((partition) + 1) << 16)

/* Synthetic devfs node kinds.  They are stored in storage_node.first_cluster
 * for device nodes; filesystem nodes continue to use that field as their
 * backend cluster/inode identifier. */
#define STORAGE_DEV_KIND_DIR       1u
#define STORAGE_DEV_KIND_NULL      2u
#define STORAGE_DEV_KIND_ZERO      3u
#define STORAGE_DEV_KIND_FULL      4u
#define STORAGE_DEV_KIND_RANDOM    5u
#define STORAGE_DEV_KIND_URANDOM   6u
#define STORAGE_DEV_KIND_TTY       7u
#define STORAGE_DEV_KIND_CONSOLE   8u
#define STORAGE_DEV_KIND_PTMX      9u
#define STORAGE_DEV_KIND_FB0       10u
#define STORAGE_DEV_KIND_KEYBOARD  11u
#define STORAGE_DEV_KIND_MOUSE     12u
#define STORAGE_DEV_KIND_AUDIO     13u
#define STORAGE_DEV_KIND_SERIAL    14u
#define STORAGE_DEV_KIND_DISK      15u
#define STORAGE_DEV_KIND_INPUT_DIR 16u
#define STORAGE_DEV_KIND_PTS_DIR   17u
#define STORAGE_DEV_KIND_NET       18u
#define STORAGE_DEV_KIND_RTC       19u
#define STORAGE_DEV_KIND_KMSG      20u
#define STORAGE_DEV_KIND_GPU         24u
#define STORAGE_DEV_KIND_SHM         25u
#define STORAGE_DEV_KIND_DISK_DIR    26u
#define STORAGE_DEV_KIND_PARTUUID_DIR 27u
#define STORAGE_DEV_KIND_DRIVERCTL 28u
#define STORAGE_DEV_KIND_SND_DIR    29u
#define STORAGE_DEV_KIND_AUDIO_CONTROL 30u
#define STORAGE_DEV_KIND_AUDIO_MIXER 31u
#define STORAGE_DEV_KIND_AUDIO_TIMER 32u
#define STORAGE_DEV_KIND_DRI_DIR      33u
#define STORAGE_DEV_KIND_DRM_CARD0    34u
#define STORAGE_DEV_KIND_DRM_RENDERD128 35u

struct boot_info;

/**
 * @brief Discover transports and native root/optical instances before VFS bootstrap.
 * BSP boot-only before tasks or AP I/O: initializes storage state, caches and
 * persistent disk records. Logging discovery does not publish a mountpoint;
 * storage_namespace_bootstrap later constructs the actual initial graph.
 */
void storage_init(void);
/**
 * @brief Toggle asynchronous I/O for the calling context on or off.
 */
void storage_set_io_async_context(bool enabled);
/* Read under the kernel execution lock; changes on writes and mount/cache resets. */
uint64_t storage_metadata_generation(void);
/**
 * @brief Abandon any in-flight I/O owned by pid without completing it.
 */
void storage_release_task_io(uint32_t pid);
/**
 * @brief Drain/discard an exiting task's I/O without dereferencing saved memory.
 * @param pid Exiting task identity, zero ignored; reaper context only under the
 * single-CPU async ownership protocol. On timeout live DMA backing remains owned
 * and quarantined by the controller, independently of the task's address space.
 */
void storage_drain_task_io(uint32_t pid);
/**
 * @brief Mount the installer's root filesystem from boot info.
 * @param boot Parsed Multiboot module list; may be NULL.
 * @return Nothing. Missing or invalid module data is logged and leaves the
 *         root volume cleared.
 * @context Boot setup before storage syscalls are available. The selected
 *          module remains reserved for the mounted volume's lifetime.
 */
void storage_init_installer_root(const struct boot_info *boot);
/**
 * @brief Mount the root filesystem for this boot.
 * @param boot Parsed Multiboot modules and kernel command line; the installer
 *             RAM disk is located by module name within it.
 * @param ramdisk_root True for an installer or live session, whose root is the
 *                     `reliefos-installer-root` module (or its legacy tag)
 *                     instead of a partition.
 * @return Nothing. A failed RAM-root mount leaves no root ready; the caller
 *         retries through storage_init_installer_root() to report why.
 * @context Boot phase, before any task can issue storage syscalls. Probes and
 *          writes disks through the storage subsystem's own locking.
 */
void storage_mount_boot_root(const struct boot_info *boot, bool ramdisk_root);
/**
 * @brief Report whether the reserved runtime-root filesystem instance is ready.
 * @return True after successful native root discovery, false before or on failure.
 * Execution-owned observation only; no I/O, allocation or ownership transfer.
 */
bool storage_ready(void);
/**
 * @brief Returns the mounted filesystem type for the runtime root volume.
 * @return Stable lowercase filesystem name, or "none" before a root mounts.
 */
const char *storage_root_filesystem_name(void);
/**
 * @brief Report whether the current runtime root is the installer RAM image.
 * @return True for an active reserved installer root, false for disk/absent root.
 * Execution-owned observation only; retains no pointer or module reference.
 */
bool storage_installer_root_active(void);
/** @brief Mount a reserved boot-module image through the standard RAM block device.
 * @param image_phys Reserved physical start; mapped only by the boot transport.
 * @param len Sector-aligned bytes, held through shutdown.
 * @return Zero or source/geometry/allocation/provider errno; failure clears the
 * unpublished root. BSP exclusive setup, before namespace users. The transport
 * borrows the direct map, never copies the whole image, and writes only session
 * memory. Filesystem/device ownership survives until final native retirement.
 */
int storage_mount_ramdisk_root(uint64_t image_phys, uint64_t len);
/**
 * @brief Read len bytes of node from offset into buf, reporting bytes read in out_read.
 */
int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buf, uint32_t len, uint32_t *out_read);
/**
 * @brief Reads a file range while reusing a cursor for sequential FAT32 I/O.
 * @param node File node to read.
 * @param offset Byte offset at which to begin the read.
 * @param buf Destination buffer.
 * @param len Maximum number of bytes to read.
 * @param out_read Receives the number of bytes copied.
 * @param cursor Optional descriptor-owned sequential-read cursor.
 * @return Zero on success or a negative storage error.
 */
int storage_read_node_cursor(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor);
/**
 * @brief Read the next directory entry of node into entry, advancing cursor; 0 on success.
 */
int storage_readdir_node(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry);
/** @brief Return a GPT UUID in a 37-byte buffer under the storage lock, or negative errno. */
int storage_disk_partition_uuid(uint32_t disk_id, uint32_t partition_index, char uuid[37]);
/** @brief Write a control file on the boot volume's actual visible attachment.
 * @param path Trusted absolute /boot/ descendant, required.
 * @param buf Kernel bytes, NULL only at length0. @param len Complete byte count.
 * @return Zero or negative errno. Standard state directories are ensured0755;
 * new files0644, existing metadata preserved. Rejects absent/overmounted ESP
 * and paths escaping its attachment. Caller authorizes under execution
 * ownership; storage synchronous, all path/writer/code pins balanced without
 * graph/module callback locks. Failure after truncation may leave a prefix.
 */
int storage_write_boot_esp_file(const char *path,const void *buf,uint32_t len);
/** @brief Unlink a literal control entry on the actual boot ESP attachment.
 * @param path Trusted absolute /boot/ descendant, required.
 * @return Zero including absent member, or negative errno. Refuses an absent
 * or overmounted ESP; final symlinks are victims, never deletion targets.
 * Caller authorizes under execution ownership; synchronous I/O, balanced pins,
 * preserved mutation latch and no graph/module locks across callbacks.
 */
int storage_unlink_boot_esp_file(const char *path);
/** @brief Adopt PID1's mounted boot ESP after an initramfs root transition.
 * @return Zero or lookup/type/ENODEV/EBUSY errno. Boot policy calls once under
 * execution ownership after mounting the root's fstab. Records only an exact
 * registered device identity; no path/code reference survives. Later control
 * operations revalidate the visible native attachment against that identity.
 * No graph/module lock spans I/O, and failed adoption leaves the binding intact.
 */
int storage_bind_boot_esp(void);
/**
 * @brief Copy discovered installer disk identities without inventing mount state.
 * @param disks Writable array of capacity entries; NULL permitted at capacity zero.
 * @param capacity Maximum records to copy, clamped to the discovered disk count.
 * @param out_count Required total-count output, including omitted records.
 * @return Zero or -EINVAL/-EAGAIN. A disk with unknown size may be identified.
 * @par Context Storage execution ownership; callers retain their output memory.
 * Actual filesystem attachments are queried through VFS, never a disk flag.
 */
int storage_install_list_disks(struct reliefos_install_disk *disks,
                               uint32_t capacity, uint32_t *out_count);
/** @brief Copy all present registered disk identities, including ephemeral RAM media.
 * @param disks Caller array, nullable at zero capacity. @param capacity Entry capacity.
 * @param out_count Required complete count, including records omitted by capacity.
 * @return Zero or validation/execution/inventory errno. Execution ownership
 * required; scalar snapshots transfer no native pointers or provider references.
 */
int storage_list_block_disks(struct reliefos_install_disk *disks,uint32_t capacity,uint32_t *out_count);
/**
 * @brief Lists GPT partitions on a detected block disk exposed to disk management.
 * @param disk_id Detected AHCI, IDE/PATA, or NVMe disk identifier.
 * @param partitions Caller buffer receiving partition metadata.
 * @param capacity Number of partition records available in @p partitions.
 * @param out_count Receives the complete number of usable GPT entries.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_list_partitions(uint32_t disk_id,
                                 struct reliefos_disk_partition *partitions,
                                 uint32_t capacity, uint32_t *out_count);
/** Returns the LBA range represented by a whole-disk or partition node. */
int storage_disk_block_info(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count);
/** Format the canonical Linux block-device path for a discovered disk. */
int storage_disk_device_name(uint32_t disk_id, int32_t partition_index,
                             char *out, uint32_t capacity);
/** Reads/writes a block node at a byte offset; offsets and lengths are sector aligned. */
/** @brief Check current mount conflicts for an exclusive whole-disk/partition open. */
int storage_disk_block_busy(uint32_t disk_id, int32_t partition_index);
int storage_disk_block_read(uint32_t disk_id, int32_t partition_index,
                            uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *out_read);
int storage_disk_block_write(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written);
/**
 * @brief Invalidate a discovered disk's partition cache after excluding active mounts.
 * @param disk_id Borrowed registered whole-disk identifier.
 * @return Zero or negative ownership/discovery/protected-root/busy errno.
 * Storage execution ownership required; no namespace or module lock spans
 * preparation I/O. Actual volume memberships, including detached held ones,
 * keep the disk busy. Cache invalidation does not transfer device ownership.
 */
int storage_disk_block_reread(uint32_t disk_id);
/**
 * @brief Formats an unprotected GPT partition as FAT32, exFAT, ext4, or legacy ext2.
 * @param request Partition selector and requested filesystem.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_format_partition(const struct reliefos_disk_partition_format *request);
/**
 * @brief Removes one unprotected GPT partition entry without wiping its data area.
 * @param request Partition selector.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_delete_partition(const struct reliefos_disk_partition_delete *request);
/**
 * @brief Creates and formats one GPT data partition in an unallocated disk range.
 * @param request Disk, filesystem, size, and display-name request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_create_partition(const struct reliefos_disk_partition_create *request);
/**
 * @brief Returns the mounted path for a runtime data partition.
 * @param disk_id AHCI, IDE/PATA, or NVMe disk identifier.
 * @param partition_index Zero-based GPT entry index.
 * @param out_path Receives the mounted absolute path.
 * @return Zero when mounted, or a negative errno-style storage error.
 */
int storage_disk_partition_mount_path(uint32_t disk_id, uint32_t partition_index,
                                      char *out_path, uint32_t capacity);
/** Returns the internal mounted-volume identity for kernel busy checks. */
int storage_disk_partition_volume_id(uint32_t disk_id, uint32_t partition_index,
                                     uint32_t *out_volume_id);
/**
 * @brief Tears down one runtime data-partition mount after the kernel checks usage.
 * @param request Disk and GPT-entry selector.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_unmount_partition(const struct reliefos_disk_partition_unmount *request);
/** Edit GPT partition type GUID and/or UTF-16 name metadata. */
int storage_disk_edit_partition(const struct reliefos_disk_partition_edit *request);
/** Initialize an empty primary/backup GPT on a managed disk. */
int storage_disk_initialize_gpt(const struct reliefos_disk_gpt_initialize *request);
/**
 * @brief Fill identity with the boot disk/model information known to storage.
 */
void storage_boot_identity(struct reliefos_machine_identity *identity);

#endif

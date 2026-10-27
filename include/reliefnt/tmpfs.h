#ifndef RELIEFNT_TMPFS_H
#define RELIEFNT_TMPFS_H
#include <reliefnt/storage.h>

/* Callers serialize these operations with the storage execution lock. */
struct tmpfs_super;
int tmpfs_new(uint32_t volume, uint64_t flags, const char *options, uint32_t uid, uint32_t gid,
              struct tmpfs_super **out);
void tmpfs_destroy(struct tmpfs_super *fs);
/**
 * @brief Create an unlinked regular inode under the storage execution lock.
 * @param fs Required filesystem owning the new inode.
 * @param uid Owner UID of the new file.
 * @param gid Owner GID of the new file.
 * @param out Required output node receiving one hold, released by tmpfs_drop.
 * @return Zero on success or negative errno; failure transfers no ownership.
 */
int tmpfs_create_anonymous(struct tmpfs_super *fs, uint32_t uid, uint32_t gid,
                            struct storage_node *out);
int tmpfs_lookup(struct tmpfs_super *fs, const char *path, struct storage_node *out);
/** @brief Look up one component using a retained directory inode, even after rename.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param parent_ino Held directory inode number in fs.
 * @param name One nonempty component, without separators or dot traversal.
 * @param out Receives borrowed node metadata; no inode reference is transferred.
 * @return Zero or negative errno. Never reconstructs a global text pathname.
 */
int tmpfs_lookup_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                         struct storage_node *out);
int tmpfs_create(struct tmpfs_super *fs, const char *path, uint32_t mode, const char *target,
                 struct storage_node *out);
/** @brief Create one tmpfs member in the caller's retained directory inode.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param parent_ino Actual parent inode, never resolved from a pathname.
 * @param name Single component within the filesystem name limit.
 * @param mode Full inode type and permission bits, already authorized.
 * @param target Borrowed symlink payload, NULL for other types. Character/block
 * nodes default to device zero; use tmpfs_mknod_child for explicit st_rdev.
 * @param value Optional owner/group; NULL preserves the legacy root owner.
 * @param out Optional metadata result; no inode reference is transferred.
 * @return Zero or negative errno. Deleted parents reject creation with ENOENT;
 * allocation failure leaves no published member. No namespace lookup or I/O.
 */
int tmpfs_create_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                           uint32_t mode,const char *target,const struct reliefos_permissions *value,
                           struct storage_node *out);
/** @brief Create a character/block inode with an explicit Linux stat device number.
 * @param fs Borrowed live filesystem under exclusive execution ownership.
 * @param parent_ino Linked parent directory inode, required.
 * @param name Single non-special component, borrowed for this call.
 * @param mode S_IFCHR or S_IFBLK and permission bits, already authorized.
 * @param device Linux encoded st_rdev; no hardware access/binding is implied.
 * @param value Optional owner/group; NULL selects root ownership.
 * @param out Optional borrowed node metadata, never an owned inode hold.
 * @return Zero or negative input/type/readonly/existence/allocation errno.
 * Failure publishes nothing; success retains the inode identity separately
 * from its device number. No I/O, locks or device callbacks occur.
 */
int tmpfs_mknod_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,
                         uint32_t mode,uint64_t device,const struct reliefos_permissions *value,
                         struct storage_node *out);
/** @brief Replace a private symlink inode payload before archive publication.
 * @param fs Borrowed filesystem under exclusive execution ownership.
 * @param ino Existing symlink inode, possibly shared by hardlinks.
 * @param target Borrowed nonempty NUL-terminated target shorter than PATH_MAX.
 * @return Zero or negative input/type/readonly/allocation errno. Allocates
 * before replacing so failure preserves old bytes; no VFS dentry changes or
 * I/O. Only private construction may mutate an existing symlink payload.
 */
int tmpfs_rewrite_symlink(struct tmpfs_super *fs,uint32_t ino,const char *target);
int tmpfs_read(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, void *buffer, uint32_t length,
               uint32_t *done);
/** @brief Read an actual inode using this access's attachment policy.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Retained regular inode; removed but held inodes remain readable.
 * @param offset Byte position. @param buffer Destination, nullable at length0.
 * @param length Requested bytes. @param done Optional actual count output.
 * @param mount_flags Current mount policy combined with shared readonly state.
 * @return Zero or negative inode/type/input errno. Updates atime according to
 * this access, without changing shared filesystem flags. No allocation/I/O.
 */
int tmpfs_read_with_flags(struct tmpfs_super *fs,uint32_t ino,uint64_t offset,void *buffer,
                            uint32_t length,uint32_t *done,uint64_t mount_flags);
int tmpfs_write(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, const void *buffer, uint32_t length,
                uint32_t *done);
int tmpfs_truncate(struct tmpfs_super *fs, uint32_t ino, uint64_t length);
/** @brief Reserve zeroed physical backing for a regular file byte range.
 * @param fs Borrowed live filesystem under the storage execution transaction.
 * @param ino Existing inode number; open unlinked files remain valid.
 * @param mode Zero to extend EOF, or Linux FALLOC_FL_KEEP_SIZE (1).
 * @param offset Nonnegative signed-off_t start byte.
 * @param length Positive byte count with a signed-off_t representable end.
 * @return Zero or EINVAL/EFBIG/EOPNOTSUPP/EROFS/ENOENT/EISDIR/ENOSPC/ENOMEM.
 * Success reserves every intersecting page without overwriting old data.
 * Failure releases only newly allocated pages, preserving size and old bytes;
 * no provider, graph or transport lock is held during allocation.
 */
int tmpfs_fallocate(struct tmpfs_super *fs,uint32_t ino,uint32_t mode,uint64_t offset,uint64_t length);
/* Returns a retained file page. The caller releases it with mm_free_page(). */
int tmpfs_get_page(struct tmpfs_super *fs, uint32_t ino, uint64_t offset, uint64_t *phys);
int tmpfs_readdir(struct tmpfs_super *fs, uint32_t ino, uint64_t *offset, struct reliefos_dir_entry *out);
/** @brief Enumerate one actual directory using this attachment's atime policy.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Retained linked directory inode.
 * @param offset Required opaque cursor, advanced only for a returned entry.
 * @param out Required entry destination. @param mount_flags Current mount/SB flags.
 * @return One entry, zero EOF or negative inode/type errno. No allocation or
 * I/O; noatime/nodiratime/readonly suppress access-time changes independently
 * of other mounts of this same filesystem. No ownership transfer.
 */
int tmpfs_readdir_with_flags(struct tmpfs_super *fs,uint32_t ino,uint64_t *offset,
                               struct reliefos_dir_entry *out,uint64_t mount_flags);
int tmpfs_unlink(struct tmpfs_super *fs, const char *path, bool directory);
int tmpfs_link(struct tmpfs_super *fs, const char *old_path, const char *new_path);
int tmpfs_rename(struct tmpfs_super *fs, const char *old_path, const char *new_path);
/** @brief Remove one member from an actual retained directory inode.
 * @param fs Borrowed live filesystem, serialized by execution ownership.
 * @param parent_ino Directory inode number, independent of its current name.
 * @param name Single component, without separators or dot/dot-dot.
 * @param directory Select rmdir rather than unlink type/empty checks.
 * @return Zero or negative errno. Open inode holds survive final unlink;
 * no pathname replay, allocation or reference transfer is performed.
 */
int tmpfs_unlink_child(struct tmpfs_super *fs,uint32_t parent_ino,const char *name,bool directory);
/** @brief Add a hardlink from one retained inode to an actual directory.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param ino Existing linked non-directory inode number in fs.
 * @param parent_ino Destination directory inode number.
 * @param name One non-special component.
 * @return Zero or negative errno. Allocation/quota failures publish nothing;
 * no namespace lookup or ownership transfer occurs.
 */
int tmpfs_link_child(struct tmpfs_super *fs,uint32_t ino,uint32_t parent_ino,const char *name);
/** @brief Rename between retained directory inodes without reconstructing paths.
 * @param fs Borrowed live filesystem under execution ownership.
 * @param old_parent_ino Source directory inode number.
 * @param old_name Source member component.
 * @param new_parent_ino Destination directory inode number.
 * @param new_name Destination member component.
 * @return Zero or negative errno. Checks cycles/types/nonempty replacement and
 * allocates before changing either directory. Existing inode holds survive
 * replacement; no pathname lookup or reference transfer is performed.
 */
int tmpfs_rename_child(struct tmpfs_super *fs,uint32_t old_parent_ino,const char *old_name,
                         uint32_t new_parent_ino,const char *new_name);
int tmpfs_readlink(struct tmpfs_super *fs, uint32_t ino, void *buffer, uint32_t length, uint32_t *done);
int tmpfs_hold(struct tmpfs_super *fs, uint32_t ino);
void tmpfs_drop(struct tmpfs_super *fs, uint32_t ino);
int tmpfs_stat(struct tmpfs_super *fs, uint32_t ino, struct linux_stat_abi *out);
int tmpfs_permissions(struct tmpfs_super *fs, uint32_t ino, struct reliefos_permissions *value, bool write);
int tmpfs_utimens(struct tmpfs_super *fs, uint32_t ino, int64_t atime, int64_t mtime, bool set_atime,
                  bool set_mtime);
void tmpfs_statfs(struct tmpfs_super *fs, struct linux_statfs_abi *out);
void tmpfs_set_flags(struct tmpfs_super *fs, uint64_t flags);
#endif

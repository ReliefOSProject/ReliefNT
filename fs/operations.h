/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_FILESYSTEM_OPERATIONS_H
#define RELIEFNT_STORAGE_FILESYSTEM_OPERATIONS_H
#include <reliefnt/types.h>
#include <reliefnt/storage.h>
struct storage_volume;
struct module;
struct fs_mount_request;
struct vfs_mount;
struct storage_cluster_ops;
#define STORAGE_FS_READONLY 1u
#define STORAGE_FS_SIDECAR 2u

/* Every operand is an execution-owned retained object or validated literal
 * component. target may be NULL; committed reports irreversible native rename
 * even when later persistence returns an error. No pointer may escape the call. */
struct storage_rename_args {
    const struct storage_node *source,*old_parent,*new_parent,*target;
    const char *old_name,*new_name;
    bool *committed;
};

/* Per-instance native codec entry points. The volume and all node arguments
 * are borrowed and pinned by the calling mount/inode. All calls require the
 * storage execution transaction; component lookup/readlink run with this
 * exact volume selected. No module/namespace spinlock may span callbacks.
 * root/lookup fill caller-owned metadata, readlink reports an exact byte count;
 * Metadata/data callbacks return zero or negative errno; readdir returns one
 * entry, zero EOF or negative errno. No borrowed node is transferred.
 * Names and this table remain immutable while any instance uses the provider.
 */
struct storage_filesystem_ops {
    const struct storage_cluster_ops *cluster;
    uint32_t flags;
    const char *name;
    struct module *owner;
    int (*mount)(struct storage_volume *volume);
    void (*destroy)(struct storage_volume *volume);
    int (*root)(struct storage_volume *volume,struct storage_node *out);
    int (*lookup)(struct storage_volume *volume,const struct storage_node *parent,
                    const char *name,struct storage_node *out);
    int (*readlink)(struct storage_volume *volume,const struct storage_node *node,
                      char *bytes,uint32_t capacity,uint32_t *length);
    int (*read)(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                   void *bytes,uint32_t capacity,uint32_t *length,
                   struct storage_read_cursor *cursor,uint64_t mount_flags);
    int (*readdir)(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                      struct reliefos_dir_entry *entry,uint64_t mount_flags);
    int (*permissions)(struct storage_volume *volume,const struct storage_node *node,
                          struct reliefos_permissions *value,bool write);
    int (*stat)(struct storage_volume *volume,const struct storage_node *node,struct linux_stat_abi *out);
    int (*utimens)(struct storage_volume *volume,const struct storage_node *node,int64_t atime,int64_t mtime,
                      bool set_atime,bool set_mtime);
    int (*statfs)(struct storage_volume *volume,struct linux_statfs_abi *out);
    int (*hold_inode)(struct storage_volume *volume,const struct storage_node *node);
    int (*put_inode)(struct storage_volume *volume,const struct storage_node *node,bool unlinked);
    int (*write)(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
                    const void *bytes,uint32_t length,uint32_t *written);
    int (*truncate)(struct storage_volume *volume,struct storage_node *node,uint64_t length);
    int (*create)(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                     const struct reliefos_permissions *value,const char *target,struct storage_node *out);
    int (*remove)(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                     const struct storage_node *node,bool directory);
    int (*rename)(struct storage_volume *volume,const struct storage_rename_args *args);
    int (*link)(struct storage_volume *volume,const struct storage_node *source,
                   const struct storage_node *parent,const char *name);
    int (*fallocate)(struct storage_volume *volume,struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length);
    int (*sync)(struct storage_volume *volume);
    /* Global native cache notifications: IRQ-safe, no allocation/I/O, may run
     * under the physical transport lock. Instance/type pins keep code alive. */
    void (*invalidate)(void);
    void (*invalidate_write)(const struct storage_volume *writer,uint64_t lba,uint32_t sectors);
};

/* Only the owning filesystem's parser/constructor installs its table. The
 * generic VFS dispatches through volume->fs_ops, never through these symbols.
 * TMPFS is permanent core (memfd/SysV/shared initial root), not a tristate.
 */
extern const struct storage_filesystem_ops storage_fat_ops;
extern const struct storage_filesystem_ops storage_exfat_ops;
extern const struct storage_filesystem_ops storage_ext_ops;
extern const struct storage_filesystem_ops storage_iso_ops;
extern const struct storage_filesystem_ops storage_tmpfs_ops;
/** @brief Acquire and construct a registered native instance before publication.
 * @param volume Exclusive empty native context with initialized transport geometry.
 * @param name Required registered filesystem name.
 * @return Zero with a volume-owned provider reference or negative errno after
 * full codec rollback. Execution ownership required; no registry lock spans
 * parsing/I/O. Raw on-disk parsers do not acquire their own code references.
 */
int storage_mount_filesystem(struct storage_volume *volume,const char *name);
/** @brief Retire an unmounted native instance and consume its provider reference.
 * @param volume Exclusive context without mounts or external inode users.
 * @return None. Caller has completed sync/purge; destroy must not fail or queue
 * work. Task context under execution ownership, no registry/module lock.
 */
void storage_destroy_filesystem(struct storage_volume *volume);
/** @brief Retire native codec then exact device ownership after final sync.
 * @param volume Exclusive quiescent instance, nullable. Execution ownership
 * required, no inode/mount/cache users remain and no callback lock is held.
 * Parser retries use codec-only destruction to retain the underlying device.
 */
void storage_destroy_volume(struct storage_volume *volume);
/** @brief Construct a registered native block mount from an actual device node.
 * @param name Required filesystem name supplied by its factory.
 * @param request Borrowed kernel operands with resolved block source.
 * @param out Required owned mount output, empty on error.
 * @return Zero or negative input/unsupported/provider errno. Execution
 * ownership and factory provider pin required across synchronous block I/O.
 */
int storage_block_get_tree(const char *name,const struct fs_mount_request *request,
                             struct vfs_mount **out);
/** @brief Read a retained native inode under one attachment's exact policy.
 * @param node Borrowed pinned actual file. @param offset Byte position.
 * @param bytes Required validated destination. @param capacity Available bytes.
 * @param length Optional actual-count output, zeroed on entry.
 * @param cursor Optional caller-owned optimization, never an inode identity.
 * @param mount_flags Effective attachment/shared flags for this access only.
 * @return Zero or negative provider/input errno. Execution transaction is
 * entered recursively; no graph/module lock spans provider I/O. Saves native
 * selection, refreshes inode size and bounds transfer before dispatch.
 */
int storage_read_node_policy(const struct storage_node *node,uint64_t offset,void *bytes,
                               uint32_t capacity,uint32_t *length,struct storage_read_cursor *cursor,
                               uint64_t mount_flags);
/** @brief Enumerate a retained native directory with attachment-local policy.
 * @param node Borrowed pinned directory. @param cursor Required opaque cursor.
 * @param entry Required metadata output. @param mount_flags Effective flags.
 * @return One entry, zero EOF or negative errno. Recursively enters execution,
 * restores selection and retains no metadata pointer across provider I/O.
 */
int storage_readdir_node_policy(const struct storage_node *node,uint64_t *cursor,
                                  struct reliefos_dir_entry *entry,uint64_t mount_flags);
/** @brief Notify each pinned native provider once about global/raw cache changes.
 * @param writer Borrowed stable raw/native writer, or NULL for global invalidation.
 * @param lba First physical sector; ignored for NULL writer.
 * @param sectors Written sector count; ignored for NULL writer.
 * @return None. Caller owns execution and every instance lifetime; may run under
 * the transport lock. Only IRQ-safe nonallocating/non-I/O callbacks are allowed;
 * physical aliases are passed even when the writer has no filesystem itself.
 */
void storage_filesystem_cache_event(const struct storage_volume *writer,uint64_t lba,uint32_t sectors);
#endif

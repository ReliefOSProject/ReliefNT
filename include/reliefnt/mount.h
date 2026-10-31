/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MOUNT_H
#define RELIEFNT_MOUNT_H
#include <reliefnt/storage.h>
#include <reliefnt/types.h>
#include <linux/mount.h>

/* Native attachment policy and shared superblock state have distinct owners.
 * MS_STRICTATIME is an input selector, represented by cleared atime bits. */
#define VFS_MOUNT_POLICY_MASK (MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME|MS_NODIRATIME|MS_RELATIME)
#define VFS_SUPER_POLICY_MASK (MS_RDONLY|MS_SYNCHRONOUS|MS_DIRSYNC|MS_LAZYTIME)

struct module;
struct vfs_mount;
struct vfs_dentry;
struct vfs_rename;
struct vfs_path;
struct vfs_walk;
struct vfs_open_file;
struct reliefos_permissions;

/** @brief Capture authorized per-OFD provider state, owning its provider path.
 * @param path Borrowed actual inode authorized before entry.
 * @param flags Sanitized status flags; O_PATH callers bypass this interface.
 * @param out Required owned output; stateless providers return NULL.
 * @return Zero or negative errno with empty output and balanced rollback.
 * Nonblocking capture under execution ownership, without graph/module locks.
 * Canonical OFD sharing also shares this handle; final close consumes it once.
 */
int vfs_file_open(const struct vfs_path *path,uint32_t flags,struct vfs_open_file **out);
/** @brief Read through captured state independently of later inode identities.
 * @param file Borrowed live per-open provider handle.
 * @param offset Positional byte offset, no ownership transfer.
 * @param buffer Kernel-accessible destination, nullable at zero capacity.
 * @param capacity Destination byte count.
 * @param read Required actual count output, cleared on validation failure.
 * @return Provider status or negative errno with bounded output count.
 * Execution ownership required; no graph/module lock spans callbacks.
 */
int vfs_file_read(struct vfs_open_file *file,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
/** @brief Consume one captured open state before its path/code can retire.
 * @param file Owned optional handle, NULL is ignored; caller clears its slot.
 * @return None. Final OFD cleanup only, after all in-flight reads finish.
 * Execution ownership required; no graph/module lock spans provider cleanup.
 */
void vfs_file_close(struct vfs_open_file *file);

/* Native namespace objects. Callers serialize the graph with the reentrant
 * kernel execution transaction. No mount-registry spinlock spans backend I/O.
 * A backend must retain actual inode objects, including unlinked directories;
 * names and formatted paths never substitute for object lifetime.
 * lookup returns borrowed metadata; retain transfers one opaque owned pin only
 * on success, and release consumes exactly that pin after dentry retirement. */
struct vfs_backend_ops {
    int (*lookup)(void *context,const struct storage_node *parent,
                  const char *name,struct storage_node *out);
    int (*retain)(void *context,const struct storage_node *node,void **pin);
    void (*release)(void *context,const struct storage_node *node,void *pin);
    bool (*same_node)(void *context,const struct storage_node *left,
                      const struct storage_node *right);
    int (*readlink)(void *context,const struct storage_node *node,
                    char *buffer,size_t capacity,size_t *length);
    /* For explicitly marked magic links only: authorize the current caller
     * before returning one independently owned target path. No textual
     * pathname replay; an anonymous file may belong to an unmounted backend.
     * Error output must be empty. No graph lock spans this provider callback. */
    int (*follow_link)(void *context,const struct storage_node *node,struct vfs_path *out);
    /* Optional final backend quiescence, after every root/inode path retires.
     * Zero permits destroy; a negative error preserves the context and its
     * module pin in the core's detached retirement queue. Failure must leave
     * state retryable and must not free context or retain stack-backed I/O.
     * The core calls once per bounded retry batch under execution ownership,
     * without a graph/module lock. No callback is made on creation failure. */
    int (*finish)(void *context);
    /* Optional owned-context teardown. After successful mount creation, runs
     * once after every retained inode/root is released and before the provider
     * module pin drops, following successful finish when supplied.
     * Creation failures leave context ownership with caller.
     * Task context without a graph spinlock; may perform backend I/O. */
    void (*destroy)(void *context);
    /* Preflight the final attachment's non-path backing references. Pure
     * execution-owned query, no I/O/mutation. Root pin is borrowed; a failed
     * check leaves the entire namespace attachment intact. */
    int (*can_unmount)(void *context,void *root_pin);
    /* Apply shared superblock policy before returning success; failures
     * preserve old flags. Attachment policy belongs to VFS, not context.
     * Caller separately authorizes and owns this mount. */
    int (*remount)(void *context,uint64_t flags);
    /* Optional positional I/O through the retained backend inode. Callers
     * retain the mount/provider while these callbacks run without graph locks.
     * mount_flags is the current attachment policy plus shared superblock
     * state, supplied per access; never change shared state for one reader. */
    int (*read)(void *context,const struct storage_node *node,uint64_t offset,
                void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags);
    /* Optional sequential-read optimization. The OFD owns cursor; callback
     * validates/invalidate it when offsets or inode data change. It is never
     * authority for an inode or mount and may be reset at any time. */
    int (*read_cursor)(void *context,const struct storage_node *node,uint64_t offset,
                        void *buffer,uint32_t capacity,uint32_t *read,
                        struct storage_read_cursor *cursor,uint64_t mount_flags);
    /* Optional OFD-local capture/read/release trio. open is nonblocking and
     * returns NULL when this inode needs no private state. Any returned state,
     * including on error, is owned by the caller and consumed by file_close.
     * The retained provider path outlives every callback; no graph lock spans
     * callbacks. Authorization precedes open, and O_PATH bypasses the trio. */
    int (*file_open)(void *context,const struct storage_node *node,uint32_t flags,void **state);
    int (*file_read)(void *context,const struct storage_node *node,void *state,
                     uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags);
    void (*file_close)(void *context,const struct storage_node *node,void *state);
    int (*readdir)(void *context,const struct storage_node *node,uint64_t *cursor,
                   struct reliefos_dir_entry *entry,uint64_t mount_flags);
    /* The held parent/name is available for filesystems whose persistent
     * ownership metadata lives in a directory sidecar. Never replay a global
     * path; callbacks run under the caller's execution transaction. */
    int (*permissions)(void *context,const struct vfs_path *path,
                       struct reliefos_permissions *value,bool write);
    /* Pure shared-state query. Initial creation also snapshots legacy/default
     * attachment bits once; later policy queries use only shared flags. */
    uint64_t (*mount_flags)(void *context);
    /* Create one member with final type/metadata before returning. Optional
     * pin receives one owned native retain, or the new member is rolled back
     * on failure. No namespace routing or EAGAIN after mutation commits. */
    int (*create_node)(void *context,const struct storage_node *parent,const char *name,uint32_t type,
                       const struct reliefos_permissions *value,const char *target,struct storage_node *out,void **pin);
    int (*getattr)(void *context,const struct vfs_path *path,struct linux_stat_abi *out);
    int (*statfs)(void *context,const struct storage_node *node,struct linux_statfs_abi *out);
    int (*write)(void *context,struct storage_node *node,uint64_t offset,
                 const void *bytes,uint32_t length,uint32_t *written);
    int (*truncate)(void *context,struct storage_node *node,uint64_t length);
    int (*fallocate)(void *context,struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length);
    /* filesystem selects whole-superblock sync. With false, unsupported
     * inode fsync returns EINVAL. data_only permits, but never requires,
     * omitting metadata unrelated to retrieval. Callbacks retain their owner. */
    int (*sync)(void *context,const struct storage_node *node,bool filesystem,bool data_only);
    /* Optional d_dname-style label for an anonymous held inode. Diagnostic
     * text only, NUL-terminated; the provider remains pinned by its path. */
    int (*display_name)(void *context,const struct storage_node *node,char *buffer,size_t capacity);
    int (*utimens)(void *context,const struct storage_node *node,int64_t atime,int64_t mtime,
                   bool set_atime,bool set_mtime);
    int (*remove_node)(void *context,const struct vfs_path *path,bool directory);
    /* Rename actual entries, retaining a replaced inode through completion.
     * committed is set once the namespace change happened, including an EIO
     * during later metadata persistence. Never request retry after commitment. */
    int (*rename_node)(void *context,const struct vfs_path *source,const struct vfs_path *parent,
                       const char *name,const struct vfs_path *target,bool *committed);
    int (*link_node)(void *context,const struct storage_node *source,
                     const struct storage_node *parent,const char *name);
};
struct vfs_path { struct vfs_mount *mount;struct vfs_dentry *dentry; };
/** @brief Compare the actual pinned inode identity, including hardlink aliases.
 * @param left Borrowed retained path, nullable/empty means no identity.
 * @param right Borrowed retained path, nullable/empty means no identity.
 * @return True for the same inode in the same provider instance. Names and
 * dentry equality are not authority. Serialized task context, no I/O or puts.
 */
bool vfs_path_same_inode(const struct vfs_path *left,const struct vfs_path *right);
/** @brief Compare the actual filesystem instances retained by two paths.
 * @param left Borrowed live path, nullable.
 * @param right Borrowed live path, nullable.
 * @return True for the same provider operations and mounted backend context,
 * including distinct attachments of one filesystem; false for invalid paths.
 * Execution-owned, allocation-free, no callback or name lookup. This does not
 * advertise optional cross-filesystem copy/reflink support.
 */
bool vfs_path_same_filesystem(const struct vfs_path *left,const struct vfs_path *right);
/** @brief Borrow the shared namespace containing an owned path.
 * @param path Borrowed retained path, nullable; serialized task context.
 * @return Namespace or NULL for an invalid/detached anonymous mount. The
 * caller must retain its path and may not persist the bare pointer. No I/O,
 * graph traversal, locking or ownership transfer occurs.
 */
struct vfs_namespace *vfs_path_namespace(const struct vfs_path *path);
struct vfs_namespace {struct vfs_mount *root,*mounts;uint64_t next_id;};
/** @brief Assign immutable diagnostic identity before publishing a mount.
 * @param mount Owned detached mount, required; attachment returns EBUSY.
 * @param source Nonempty kernel source label, copied with PATH_MAX bounds.
 * @param filesystem Nonempty kernel filesystem name, copied with NAME_MAX bounds.
 * @param device Linux encoded st_dev of this filesystem instance.
 * @return Zero or negative input/allocation errno. Replacement is atomic on
 * allocation failure. Names never select a backend or authorize operations.
 * Serialized task context, no I/O or locks; mount retirement frees the copy.
 */
int vfs_mount_describe(struct vfs_mount *mount,const char *source,const char *filesystem,uint64_t device);
/** @brief Render proc mounts/mountinfo from the actual referenced mount graph.
 * @param ns Borrowed execution-owned namespace, required.
 * @param root Retained viewing root; mounts outside it are omitted.
 * @param mountinfo True for mountinfo, false for mntent format.
 * @param offset Byte offset into this live view.
 * @param buffer Destination, nullable only for zero capacity.
 * @param capacity Maximum bytes to copy.
 * @param read Optional output count, cleared even on error.
 * @return Zero or negative input/namespace/allocation/format errno. IDs and
 * parentage come from the graph; names are escaped and reflect current moves.
 * Visible parents precede their children even after MS_MOVE changes ancestry.
 * Providers stay pinned by their mounts. Uses a temporary PATH_MAX workspace;
 * no graph spinlock or new inode I/O spans this serialized read.
 */
int vfs_read_mount_table(const struct vfs_namespace *ns,const struct vfs_path *root,bool mountinfo,
                            uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
/** @brief Format an attached backend's current namespace mountpoint for display.
 * @param ns Borrowed live namespace, under execution ownership.
 * @param context Borrowed non-NULL provider context identifying the backend.
 * @param buffer Required diagnostic output, cleared on failure.
 * @param capacity Bytes including the terminating NUL.
 * @return Zero, ENOENT if detached/absent, or negative input/format errno.
 * Multiple attachments choose the smallest stable mount ID. No ownership
 * changes, allocation, provider callback or I/O occurs. This text carries no
 * authority for backend selection, opening, or unmounting an inode.
 */
int vfs_namespace_context_mountpoint(const struct vfs_namespace *ns,const void *context,char *buffer,size_t capacity);
/** @brief Remount the exact retained attachment rather than a formatted name.
 * @param path Retained mounted filesystem root, required.
 * @param flags Supported final filesystem/mount policy, excluding MS_REMOUNT.
 * @return Zero or negative type/unsupported/busy/provider errno. Provider
 * validates before committing; caller separately checks CAP_SYS_ADMIN.
 * Execution-owned task context without graph/module locks across callbacks.
 */
int vfs_path_remount(const struct vfs_path *path,uint64_t flags);
struct vfs_boot_mount {
    const char *target;
    /* Transfer one detached owned mount only on success; callbacks/context
     * are borrowed for this synchronous bootstrap and run without graph locks. */
    int (*create)(void *context,struct vfs_mount **out);
    void *context;
    /* Create only a missing final resolved directory, root-owned 0755. Parent
     * directories must already exist; existing inode metadata is preserved. */
    bool create_directory;
};
/** @brief Assemble and publish the system's shared initial mount namespace.
 * @param create_root Factory returning one owned detached initial root mount.
 * @param root_context Borrowed root factory context.
 * @param mounts Ordered initial attachments; parents must precede children,
 * NULL only with count zero. Target strings live through this call only.
 * @param count Number of attachment descriptors.
 * @return Zero or negative errno. Publishes only after all factories and
 * attachments succeed; failures retire every temporary path and mount. A newly
 * created empty target directory may remain after a later assembly failure.
 * Called
 * before task creation under exclusive execution ownership, without scheduler,
 * graph or module locks across I/O. An existing namespace returns EBUSY.
 */
int vfs_system_bootstrap(int (*create_root)(void *,struct vfs_mount **),void *root_context,
                            const struct vfs_boot_mount *mounts,size_t count);
/** @brief Borrow the one shared namespace after initial assembly succeeds.
 * @return Live namespace or NULL before publication/after final destruction.
 * Caller holds execution ownership; the pointer does not add a reference or
 * create a private namespace. Paths retain individual mounts independently.
 */
struct vfs_namespace *vfs_system_namespace(void);
/** @brief Look up one member of the exact retained backend directory.
 * @param parent Borrowed directory retaining the provider through lookup I/O.
 * @param name Borrowed single kernel component, excluding dot/dot-dot.
 * @param out Empty output receiving one owned path, empty on failure.
 * @return Zero or negative errno. Does not follow a symlink or covering mount;
 * namespace traversal/search authorization belongs to the caller. Deleted
 * directories reject new lookups. No graph lock spans provider callbacks.
 */
int vfs_path_lookup_child(const struct vfs_path *parent,const char *name,struct vfs_path *out);
/** @brief Query the retained inode's native metadata and actual ownership.
 * @param path Borrowed owned path under execution ownership.
 * @param out Required Linux stat output, empty on failure.
 * @return Zero or negative errno; no pathname replay, access check or graph
 * lock across provider I/O. Native timestamps, links and inode numbers survive
 * rename/unlink; the caller separately encodes device-specific rdev values.
 */
int vfs_path_stat(const struct vfs_path *path,struct linux_stat_abi *out);
/** @brief Obtain native inode/type metadata for one enumerated directory member.
 * @param directory Retained directory authorizing the enumeration, required.
 * @param name Borrowed NUL-terminated backend component, including dot/dot-dot.
 * @param out Required Linux stat output, cleared on error.
 * @return Zero or negative lookup/provider errno. Does not follow a symlink
 * or covering mount. Dot-dot uses the filesystem dentry parent; a filesystem
 * root reports itself, independently of its namespace mountpoint/chroot.
 * A temporary path pins provider code through metadata I/O. Caller owns the
 * execution transaction; no graph/module spinlock spans callbacks.
 */
int vfs_path_dirent_stat(const struct vfs_path *directory,const char *name,struct linux_stat_abi *out);
/** @brief Query the actual filesystem backing a retained path.
 * @param path Borrowed path retaining its provider.
 * @param out Required Linux statfs output, empty on failure.
 * @return Zero or negative errno; no namespace lookup or lock across I/O.
 */
int vfs_path_statfs(const struct vfs_path *path,struct linux_statfs_abi *out);
/** @brief Write through a retained inode after caller DAC/writer admission.
 * @param path Borrowed file path retaining its provider.
 * @param offset Nonnegative byte offset.
 * @param bytes Readable kernel/validated user bytes, nullable only for zero length.
 * @param length Byte count.
 * @param written Required completed-byte output, zero on pre-I/O failure.
 * @return Zero or negative errno; short completion is preserved, impossible
 * counts fail EIO. Caller serializes mutation without graph/module spinlocks.
 */
int vfs_path_write(const struct vfs_path *path,uint64_t offset,const void *bytes,
                      uint32_t length,uint32_t *written);
/** @brief Resize a retained regular file after DAC and writer admission.
 * @param path Borrowed file path retaining its provider.
 * @param length New nonnegative logical size.
 * @return Zero or negative errno. No pathname replay or graph lock across I/O;
 * directory/type/readonly checks happen before the provider callback.
 */
int vfs_path_truncate(const struct vfs_path *path,uint64_t length);
/** @brief Preallocate an authorized retained regular inode's actual backing.
 * @param path Borrowed path retaining its provider through allocation/I/O.
 * @param mode Linux fallocate flags, interpreted by the provider.
 * @param offset Nonnegative starting byte.
 * @param length Positive range length with signed-off_t representable end.
 * @return Zero or negative errno; validates type/range/readonly before the
 * provider callback. Caller holds writer admission under execution ownership;
 * no namespace lookup or graph/module spinlock spans allocation or I/O.
 */
int vfs_path_fallocate(const struct vfs_path *path,uint32_t mode,uint64_t offset,uint64_t length);
/** @brief Synchronize an actual held file or its containing filesystem.
 * @param path Borrowed path retaining the provider during synchronous I/O.
 * @param filesystem True for superblock syncfs, false for inode fsync.
 * @param data_only True permits omitting retrieval-independent metadata.
 * @return Zero or provider errno, including device flush errors; unsupported
 * inode fsync is EINVAL, a filesystem without persistent state succeeds.
 * Execution ownership required; no graph/module lock spans callbacks.
 */
int vfs_path_sync(const struct vfs_path *path,bool filesystem,bool data_only);
/** @brief Complete an implicit synchronous write to the retained inode.
 * @param path Required borrowed held file path retaining its provider.
 * @param data_only True for data-only synchronization.
 * @return Zero or negative provider errno. An immediately committed proc
 * diagnostic needs no second flush; explicit fsync keeps its own semantics.
 * Execution ownership required; no namespace replay or graph lock across I/O. */
int vfs_path_sync_after_write(const struct vfs_path *path, bool data_only);
/** @brief Flush every attached filesystem in the shared namespace.
 * @param ns Borrowed initialized namespace under exclusive execution ownership.
 * @return First provider error after attempting all mounts, or zero. Backend
 * callbacks must not mutate the mount graph; no graph/module spinlock spans
 * synchronous I/O. Each callback is protected by a temporary root path pin.
 */
int vfs_namespace_sync(struct vfs_namespace *ns);
/** @brief Set timestamps on an authorized retained inode.
 * @param path Borrowed path retaining its provider across metadata I/O.
 * @param atime Access time in whole Unix seconds, matching backend resolution.
 * @param mtime Modification time in whole Unix seconds.
 * @param set_atime False to leave the access time unchanged.
 * @param set_mtime False to leave the modification time unchanged.
 * @return Zero or negative errno; readonly is checked before callbacks.
 * Caller applies owner/write/capability checks under execution ownership;
 * neither namespace lookup nor a graph lock participates in the update.
 */
int vfs_path_utimens(const struct vfs_path *path,int64_t atime,int64_t mtime,bool set_atime,bool set_mtime);
/** @brief Read an actual retained symlink without following it.
 * @param path Borrowed retained symlink.
 * @param buffer Writable byte output; no terminating NUL is added.
 * @param capacity Maximum output bytes, greater than zero.
 * @param length Required completed-byte output, zero on failure.
 * @return Zero or negative errno. Provider ownership lasts through I/O and
 * removed links remain addressable while the caller holds their inode path.
 */
int vfs_path_readlink(const struct vfs_path *path,char *buffer,size_t capacity,size_t *length);
#define VFS_LOOKUP_FOLLOW 1u
#define VFS_LOOKUP_PARENT 2u
/* Return the checked parent/name if only the final resolved member is absent.
 * Unlike PARENT, follows links according to FOLLOW, including a dangling final
 * symlink's actual target. Callers decide which creation types allow trailing /. */
#define VFS_LOOKUP_MISSING 4u
#define VFS_PATH_MAX 4096u
#define VFS_NAME_MAX 255u
#define VFS_SYMLINK_MAX 40u

struct vfs_lookup_policy {
    unsigned flags;
    /* Check search access on each actual directory before consuming a
     * component, including . and ... NULL is trusted kernel traversal. */
    int (*search)(void *context,const struct vfs_path *directory);
    void *context;
};
struct vfs_lookup_result {
    struct vfs_path path;
    char last[VFS_NAME_MAX+1];
    bool trailing;
    bool missing;
};
/** @brief Resolve a path or its creation parent with per-directory authorization.
 * @param ns Shared namespace under execution ownership.
 * @param root Held task root, used for absolute input/links and .. boundary.
 * @param start Held cwd/dirfd for relative input.
 * @param name Kernel-owned NUL-terminated input, at most VFS_PATH_MAX-1 bytes.
 * @param policy Borrowed callback/context/flags; NULL follows links as kernel.
 * @param out Empty output; owns one path on success. PARENT returns the held
 * parent plus an unresolved last component and trailing-slash indication.
 * MISSING follows links first, returning that same parent/name plus missing=true
 * only when the last resolved member is absent; an intermediate miss fails.
 * @return Zero or negative errno, releasing all temporaries on error. Search
 * callbacks run on held objects before backend lookup; no graph lock spans I/O.
 */
int vfs_path_resolve(struct vfs_namespace *ns,const struct vfs_path *root,
                       const struct vfs_path *start,const char *name,
                       const struct vfs_lookup_policy *policy,struct vfs_lookup_result *out);
/** @brief Resolve or resume the same component walk across pre-I/O EAGAIN.
 * @param ns Shared namespace under execution ownership.
 * @param root Initial held task root, borrowed and retained by a new walk.
 * @param start Initial held cwd/dirfd, borrowed and retained as current position.
 * @param name Initial copied input; a resumed walk uses its own pending text.
 * @param policy Borrowed per-call search callback/context and unchanged flags;
 * callback/context are never retained between calls.
 * @param state Empty or owned opaque walk, retained only when EAGAIN is returned.
 * @param out Empty result receiving an owned final path on success.
 * @return Zero or negative errno. EAGAIN preserves the exact current directory,
 * pending symlink and traversal phase; overmounts/renames cannot replay earlier
 * components or release in-flight provider code. Other results consume/clear
 * state. Caller must cancel on signals/exit/replacement; no graph lock spans I/O.
 */
int vfs_path_resolve_resume(struct vfs_namespace *ns,const struct vfs_path *root,
                              const struct vfs_path *start,const char *name,
                              const struct vfs_lookup_policy *policy,struct vfs_walk **state,
                              struct vfs_lookup_result *out);
/** @brief Cancel one pending walk and release every retained object and buffer.
 * @param state Owned walk pointer, nullable; cleared before backend final puts.
 * Serialized task context without scheduler, graph or module-table locks.
 * Providers may perform I/O during final release; no caller input is retained.
 */
void vfs_walk_cancel(struct vfs_walk **state);

/** @brief Create a detached filesystem mount and pin its code and root object.
 * @param ops Persistent callbacks with lookup/retain/release/same_node required.
 * @param context Backend-owned state, valid through any deferred finish retry;
 * successful creation transfers final teardown to ops.destroy when supplied.
 * @param owner FS provider; NULL is builtin, otherwise must be LIVE.
 * @param root Borrowed existing directory object, retained on success.
 * @param out Receives one owned mount reference, NULL on failure.
 * @return Zero or negative errno, no leaked root/code references on failure.
 * Task context under execution ownership; no module lock spans callbacks.
 */
int vfs_mount_new(const struct vfs_backend_ops *ops,void *context,struct module *owner,
                   const struct storage_node *root,struct vfs_mount **out);
/** @brief Drop an owned mount handle; retire its root before final teardown.
 * @param mount Owned reference or NULL; task context under execution ownership.
 * A failed finish retains context/code in the detached core retirement queue.
 * No allocation or graph/module lock spans finish/destroy callbacks.
 */
void vfs_mount_put(struct vfs_mount *mount);
/** @brief Retry a bounded batch of detached filesystem backend retirements.
 * @param budget Maximum attempts; zero performs no callbacks.
 * @return Number reclaimed. Each object pending at entry is attempted at most
 * once; failures keep context/code alive and requeue at the tail. Serialized
 * execution ownership is required; no allocation or graph/module lock spans
 * callbacks. Recursive attempts do no work. An empty queue is a constant-time
 * fast path; no namespace attachment or public path survives here.
 */
uint32_t vfs_reclaim_mounts(uint32_t budget);
/** @brief Set initial policy on one detached, owned mount without changing its backend.
 * @param mount Owned unpublished mount, required; no namespace membership.
 * @param flags Per-attachment MS flags, with optional MS_STRICTATIME input.
 * @return Zero, EINVAL/EBUSY for invalid state, or EOPNOTSUPP for other flags.
 * Execution ownership required; pure field publication, no callbacks, I/O,
 * allocation or ownership transfer. Shared readonly state remains effective.
 */
int vfs_mount_set_flags(struct vfs_mount *mount,uint64_t flags);
/** @brief Initialize an empty namespace, retaining its initial root mount.
 * @param ns Zero-initialized namespace owned by the caller.
 * @param root Detached mount, borrowed and retained; caller keeps its handle.
 * @return Zero or negative errno, no change on failure. Serialized task context.
 */
int vfs_namespace_init(struct vfs_namespace *ns,struct vfs_mount *root);
/** @brief Destroy a namespace only when every external path/handle has gone.
 * @param ns Namespace to clear; NULL is invalid.
 * @return Zero or -EBUSY without partial detach on an external reference.
 * Serialized task context; final backend callbacks can perform I/O.
 */
int vfs_namespace_destroy(struct vfs_namespace *ns);
/** @brief Get the currently visible namespace root for a new task FS state.
 * @param ns Live namespace, borrowed.
 * @param out Receives one owned path. Existing held task roots never redirect.
 * @return Zero or negative errno. Serialized task context.
 */
int vfs_namespace_root(const struct vfs_namespace *ns,struct vfs_path *out);
/** @brief Attach a detached mount at a directory object, retaining both sides.
 * @param ns Shared namespace under execution ownership.
 * @param mount Detached mount; caller retains its own handle.
 * @param target Held directory path in ns; an already covered slot is busy.
 * @return Zero or negative errno without partial publication. Task context.
 */
int vfs_mount_attach(struct vfs_namespace *ns,struct vfs_mount *mount,
                      const struct vfs_path *target);
/** @brief Atomically move an attached subtree, preserving held paths and IDs.
 * @param ns Namespace under execution ownership.
 * @param source Held root of an attached mount, never the namespace base root.
 * @param target Held destination directory path in the same namespace.
 * @return Zero or negative errno; cycles/cross-namespace/invalid targets leave
 * the old attachment unchanged. No callbacks or allocation during commit.
 */
int vfs_mount_move(struct vfs_namespace *ns,const struct vfs_path *source,
                    const struct vfs_path *target);
/** @brief Detach an unused mounted tree root using ordinary Linux busy semantics.
 * @param ns Namespace under execution ownership.
 * @param path The caller's sole external path to the mounted root.
 * @return Zero or negative errno. Children/other held paths keep it busy;
 * success retains the caller's path until vfs_path_put, no forced free.
 */
int vfs_mount_unmount(struct vfs_namespace *ns,const struct vfs_path *path);
/** @brief Prepare retained directory-name changes before a backend rename.
 * @param ns Shared namespace, serialized by the execution transaction.
 * @param source Held source dentry, excluding a mount root or covered point.
 * @param parent Held destination directory in the same mount.
 * @param name New single component, excluding dot and dot-dot.
 * @param out Receives an owned transaction; empty on failure.
 * @return Zero or negative errno. Allocates the new name and pins source,
 * parent and any replaced target. Caller must abort on backend failure,
 * including EAGAIN, or commit after backend success, without letting another
 * namespace mutation intervene. Backend permission/type checks remain required.
 */
int vfs_rename_prepare(struct vfs_namespace *ns,const struct vfs_path *source,
                         const struct vfs_path *parent,const char *name,
                         struct vfs_rename **out);
/** @brief Discard a prepared rename without changing any dentry identity.
 * @param change Owned transaction or NULL; execution transaction required.
 */
void vfs_rename_abort(struct vfs_rename *change);
/** @brief Publish a successful backend rename without allocation or I/O.
 * @param change Owned prepared transaction; consumed even for same-inode no-op.
 * Held paths follow the new parent/name. A replaced target remains pinned but
 * loses its visible name, just as an unlinked open object does.
 */
void vfs_rename_commit(struct vfs_rename *change);
/** @brief Retire a successfully removed name while preserving held objects.
 * @param path Held non-root dentry after the backend unlink/rmdir succeeded.
 * Execution transaction required; no I/O/allocation. Relative directory lookup
 * and dot-dot still use the held inode/parent; path formatting returns ENOENT.
 */
void vfs_path_unlinked(const struct vfs_path *path);
/** @brief Copy and retain a held mount/dentry path, with NULL as an empty path.
 * @param source Borrowed path under execution ownership.
 * @param out Empty destination receiving its own path; may equal source.
 */
void vfs_path_get(const struct vfs_path *source,struct vfs_path *out);
/** @brief Release one path and clear its handle, dropping unreachable dentries.
 * @param path Owned path or NULL, under execution ownership.
 */
void vfs_path_put(struct vfs_path *path);
/** @brief Compare actual mount/dentry identity, independent of formatted names.
 * @param left Borrowed path, nullable.
 * @param right Borrowed path, nullable.
 * @return True for the same valid object pair. Caller keeps both stable.
 */
bool vfs_path_equal(const struct vfs_path *left,const struct vfs_path *right);
/** @brief Borrow the backend node held by a valid path.
 * @param path Held path, nullable.
 * @return Borrowed node or NULL, valid only through the held path lifetime.
 */
const struct storage_node *vfs_path_node(const struct vfs_path *path);
/** @brief Borrow the retained final component for display or name policy.
 * @param path Borrowed path, nullable; caller keeps it stable.
 * @return Borrowed name, possibly empty for a root, or NULL if absent. It
 * remains available for disconnected dentries and never routes a namespace.
 */
const char *vfs_path_name(const struct vfs_path *path);
/** @brief Borrow the backend state belonging to a held mount.
 * @param path Held path, nullable.
 * @return Backend context or NULL. No ownership transfer.
 */
void *vfs_path_context(const struct vfs_path *path);
/** @brief Read a retained backend file without replaying a namespace pathname.
 * @param path Borrowed held file under execution ownership.
 * @param offset Positional byte offset; no OFD cursor is changed.
 * @param buffer Writable kernel destination for capacity bytes; NULL only if zero.
 * @param capacity Maximum bytes to copy.
 * @param read Required byte-count output, cleared before callback admission.
 * @return Zero or negative errno, including EISDIR/EOPNOTSUPP. Callback and code
 * remain pinned by path; no graph spinlock spans I/O. Partial counts survive a
 * backend error, but an invalid count above capacity is rejected as EIO.
 */
int vfs_path_read(const struct vfs_path *path,uint64_t offset,void *buffer,
                    uint32_t capacity,uint32_t *read);
/** @brief Read a retained inode while preserving its provider's OFD optimization.
 * @param path Borrowed actual file path retaining the provider.
 * @param offset Nonnegative positional byte offset.
 * @param buffer Writable kernel/validated user bytes, NULL only for zero capacity.
 * @param capacity Requested byte count.
 * @param read Required completed count, cleared before validation/I/O.
 * @param cursor Optional OFD-owned read hint, never an inode identity.
 * @return Zero or negative errno, preserving short/error progress. Falls back
 * to the same held provider's read callback and invalidates unsupported hints;
 * impossible counts fail EIO. Execution ownership without graph/module locks
 * across callbacks. No lookup, name replay or additional reference allocation.
 */
int vfs_path_read_cursor(const struct vfs_path *path,uint64_t offset,void *buffer,
                            uint32_t capacity,uint32_t *read,struct storage_read_cursor *cursor);
/** @brief Enumerate a retained directory independently of its global name.
 * @param path Borrowed held directory under execution ownership.
 * @param cursor Required backend cursor, updated by the provider.
 * @param entry Required writable result entry.
 * @return One for an entry, zero for EOF, or negative errno. Provider remains
 * pinned; no graph spinlock spans enumeration. Other types return ENOTDIR.
 */
int vfs_path_readdir(const struct vfs_path *path,uint64_t *cursor,struct reliefos_dir_entry *entry);
/** @brief Query or update metadata through an owned filesystem object.
 * @param path Borrowed retained path; its provider stays pinned.
 * @param value Writable result or borrowed new mode/uid/gid.
 * @param write True to update; caller must authorize credential changes first.
 * @return Zero or negative errno. Serialized task context, no graph spinlock
 * spans backend I/O. A filesystem lacking metadata support returns EOPNOTSUPP.
 */
int vfs_path_permissions(const struct vfs_path *path,struct reliefos_permissions *value,bool write);
/** @brief Read the policy of the actual mount containing a held object.
 * @param path Borrowed valid path under execution ownership.
 * @return Attachment MS bits plus current shared readonly/sync policy, or zero
 * for invalid input. Intrinsic backend readonly remains effective on all mounts.
 * No I/O or ownership transfer; caller retains provider code via path.
 */
uint64_t vfs_path_mount_flags(const struct vfs_path *path);
/** @brief Create a regular file in the actual retained parent directory.
 * @param parent Borrowed directory under execution ownership.
 * @param name Single nonempty component, excluding dot and dot-dot.
 * @param value Borrowed final authorized mode/uid/gid; caller applies umask,
 * setgid inheritance and parent search/write checks before entry.
 * @param out Empty output receiving one owned path on success.
 * @return Zero or negative errno. EEXIST never changes the existing inode.
 * A later dentry-allocation failure may leave the newly created file, as an
 * open failure after creation may; errors return no path ownership. The held
 * parent/provider stays pinned and no graph lock spans filesystem I/O.
 */
int vfs_path_new_file(const struct vfs_path *parent,const char *name,
                         const struct reliefos_permissions *value,struct vfs_path *out);
/** @brief Create a member in one actual retained directory.
 * @param parent Borrowed directory retaining the filesystem provider.
 * @param name Single non-special member component.
 * @param type S_IFREG/S_IFDIR/S_IFLNK/S_IFIFO/S_IFSOCK inode type.
 * @param value Borrowed final mode/uid/gid after caller authorization/umask.
 * @param target Borrowed symlink bytes, NULL for all other types.
 * @param out Optional empty output receiving one owned path; NULL performs
 * creation only without a later VFS allocation or inode-open step.
 * @return Zero or negative errno. Existing names remain unchanged; no global
 * path reconstruction. Backend allocation/metadata failures roll back before
 * success. Requested VFS storage is reserved before mutation; the backend
 * returns its native pin in the same operation or rolls back the new member.
 * No allocation or inode read follows successful creation.
 */
int vfs_path_create_node(const struct vfs_path *parent,const char *name,uint32_t type,
                            const struct reliefos_permissions *value,const char *target,struct vfs_path *out);
/** @brief Unlink/rmdir the exact retained directory member.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param path Borrowed victim path retaining its parent, inode and provider.
 * @param directory Select rmdir rather than unlink type/empty checks.
 * @return Zero or negative errno. Caller authorizes parent/sticky policy.
 * Rejects mount roots/covered mountpoints, retains metadata before removal,
 * and disconnects the dentry only after successful backend commit. Open paths
 * remain valid inode references; no graph lock spans provider callbacks.
 */
int vfs_path_remove(struct vfs_namespace *ns,const struct vfs_path *path,bool directory);
/** @brief Rename an actual inode and its held path graph as one operation.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param source Held directory member, retaining its provider through I/O.
 * @param parent Held destination directory in the same mounted filesystem.
 * @param name Single non-special destination component.
 * @return Zero or negative errno; checks mountpoints/cycles/readonly before
 * mutation and reserves all graph storage first. Replaced open inodes remain
 * valid. If the backend reports post-commit EIO the graph still follows the
 * committed rename, and EAGAIN is never returned for a completed side effect.
 * Caller performs parent/sticky DAC; no graph/module lock spans callbacks.
 */
int vfs_path_rename(struct vfs_namespace *ns,const struct vfs_path *source,
                       const struct vfs_path *parent,const char *name);
/** @brief Add a hardlink to a still-linked held inode in the same mount.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param source Held non-directory inode, retaining its provider through I/O.
 * @param parent Held destination directory in the same mounted filesystem.
 * @param name Single non-special destination component.
 * @return Zero or negative errno; directory/readonly/mount/existing-name and
 * zero-link checks precede creation. Filesystem metadata and inode identity
 * are shared, no dentry allocation follows creation. Caller applies DAC;
 * no graph/module lock spans provider callbacks.
 */
int vfs_path_link(struct vfs_namespace *ns,const struct vfs_path *source,
                     const struct vfs_path *parent,const char *name);
/** @brief Retain an anonymous regular inode without inventing a directory name.
 * @param mount Held backend mount, detached or attached; borrowed.
 * @param node Existing regular inode metadata, borrowed and retained on success.
 * @param out Empty output receiving one owned mount/dentry pair.
 * @return Zero or negative errno without leaked pins on allocation/retain
 * failure. Execution transaction required; backend retain may perform I/O.
 * The result cannot be renamed or formatted as a visible pathname, but may be
 * retained by an OFD and returned by an authorized proc-style magic link.
 */
int vfs_path_anonymous(struct vfs_mount *mount,const struct storage_node *node,struct vfs_path *out);
/** @brief Walk components from actual task root/cwd/dirfd paths.
 * @param ns Namespace under execution ownership.
 * @param root Held task root, used for absolute symlinks and the .. boundary.
 * @param start Held cwd/dirfd used for relative input; never redirected by covers.
 * @param name NUL-terminated input, at most VFS_PATH_MAX-1 bytes.
 * @param flags VFS_LOOKUP_FOLLOW to follow a final symlink; trailing / also follows.
 * @param out Empty output receiving one owned path, empty on failure.
 * @return Zero or negative errno. Backends may return EAGAIN before commit;
 * all temporary paths/buffers are released on failure. No mount lock spans I/O.
 */
int vfs_path_lookup(struct vfs_namespace *ns,const struct vfs_path *root,
                     const struct vfs_path *start,const char *name,unsigned flags,
                     struct vfs_path *out);
/** @brief Format a held path relative to a held root through actual attachments.
 * @param ns Namespace under execution ownership.
 * @param root Held formatting root, e.g. the calling task's chroot.
 * @param path Held path whose identity is preserved across mount moves.
 * @param buffer Writable output, cleared on failure.
 * @param capacity Output capacity, including NUL.
 * @return Zero, -ENOENT outside root, or negative errno. No backend I/O;
 * formatted text is a display/adapter value and never owns a mount.
 */
int vfs_path_format(const struct vfs_namespace *ns,const struct vfs_path *root,
                     const struct vfs_path *path,char *buffer,size_t capacity);
/** @brief Render a held object for proc magic-link diagnostics, like d_path.
 * @param ns Borrowed shared namespace, nullable for an anonymous inode.
 * @param root Held caller root; nullable for an anonymous inode. Paths outside
 * it use the namespace base root.
 * @param path Actual held object, including a renamed or unlinked dentry.
 * @param buffer Writable diagnostic output, empty on failure.
 * @param capacity Bytes including NUL and any deleted suffix.
 * @return Zero or negative errno. Appends " (deleted)" for an unlinked leaf;
 * traverses retained parent/attachment objects without lookup, I/O or locks.
 * Anonymous inodes use their provider label, or return EOPNOTSUPP when absent.
 * This text conveys no authority and must never be used to reopen the inode.
 */
int vfs_path_format_link(const struct vfs_namespace *ns,const struct vfs_path *root,
                            const struct vfs_path *path,char *buffer,size_t capacity);
/** @brief Format cwd with Linux's unreachable-root diagnostic.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param root Held process root, possibly changed independently by chroot.
 * @param cwd Held current directory, possibly outside that root.
 * @param buffer Writable result, cleared on failure.
 * @param capacity Bytes including NUL; no truncated path is returned.
 * @return Zero or negative errno. Disconnected/deleted directories return
 * ENOENT; a still-linked cwd outside root is prefixed with (unreachable).
 * Uses actual mount attachments, never replays a cached pathname or performs I/O.
 */
int vfs_path_getcwd(const struct vfs_namespace *ns,const struct vfs_path *root,
                       const struct vfs_path *cwd,char *buffer,size_t capacity);
/** @brief Format an owned object relative to its own filesystem root.
 * @param path Held mount/dentry pair under execution ownership, required.
 * @param buffer Writable backend-relative pathname, cleared on failure.
 * @param capacity Output capacity including NUL.
 * @return Zero or negative errno, including ENOENT for a disconnected name.
 * Moves/overmounts do not alter this backend adapter name. It never resolves
 * a namespace prefix or substitutes for the caller's held object reference.
 */
int vfs_path_backend_name(const struct vfs_path *path,char *buffer,size_t capacity);
#endif

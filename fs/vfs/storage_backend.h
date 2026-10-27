/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VFS_STORAGE_BACKEND_H
#define RELIEFNT_VFS_STORAGE_BACKEND_H
#include <reliefnt/mount.h>
struct storage_volume;
/* Execution-owned backing queries/retirement; the root pin is the mount's
 * single native inode reference. No name lookup or scheduler-table scan. */
bool storage_inode_volume_other_users(uint32_t volume,const struct storage_inode_ref *root);
bool storage_inode_volume_writers(uint32_t volume);
void storage_volume_mount_put(struct storage_volume *volume);
/** @brief Look up one component in an already selected filesystem directory.
 * @param context Borrowed storage volume, pinned by its VFS mount.
 * @param parent Borrowed directory node belonging to that volume.
 * @param name Borrowed component; no global pathname resolution.
 * @param out Metadata output, retained separately by the caller if needed.
 * @return Zero or negative errno; execution transaction required across I/O.
 */
int storage_backend_lookup(void *context,const struct storage_node *parent,
                               const char *name,struct storage_node *out);
/** @brief Read or change inode metadata using retained inode and parent objects.
 * @param context Borrowed mount volume.
 * @param path Borrowed held path belonging to that volume.
 * @param value Metadata result or new authorized mode/uid/gid.
 * @param write True to store, false to query.
 * @return Zero or negative errno; caller owns execution transaction and all
 * credential checks. No global namespace pathname is resolved.
 */
int storage_backend_permissions(void *context,const struct vfs_path *path,
                                  struct reliefos_permissions *value,bool write);
/* All below are explicit backing operations under execution ownership. */
/** @brief Create a native empty member after verified selected-parent lookup.
 * @param volume Borrowed pinned instance. @param parent Actual retained directory.
 * @param name Literal component. @param out Required metadata output.
 * @return Zero or negative input/existence/provider errno. Execution ownership
 * required; native insertion rolls back if stable inode allocation fails.
 * Internal sidecars use this primitive without recursively storing permissions.
 */
int storage_cluster_new_file(struct storage_volume *volume,const struct storage_node *parent,
                                const char *name,struct storage_node *out);
/* Roll back only a just-created internal/user member, never an old record. */
/** @brief Roll back only a just-created unpublished native cluster member.
 * @param volume Borrowed pinned instance. @param parent Actual retained directory.
 * @param name Newly created component. @param node Its generation-stamped metadata.
 * @return Zero or provider errno. Execution/mutation admission required; native
 * callback disconnects the inode key before backing reuse, no callback lock.
 */
int storage_cluster_drop_new(struct storage_volume *volume,const struct storage_node *parent,
                                const char *name,const struct storage_node *node);
int storage_backend_store_child(struct storage_volume *volume,const struct storage_node *parent,
                                   const char *name,const struct reliefos_permissions *value);
/** @brief Create an authorized native inode before publishing its VFS dentry.
 * @param context Borrowed pinned instance. @param parent Actual directory.
 * @param name Validated component. @param type Authorized Linux inode type.
 * @param value Borrowed authorized ownership/mode. @param target Optional symlink target.
 * @param out Required node output. @param pin Optional owned inode-pin output.
 * @return Zero or input/readonly/native/metadata errno. Execution ownership
 * required; creation, sidecar and publication-pin failures roll back before
 * exposing a dentry; no graph/module lock spans provider I/O.
 */
int storage_backend_create_node(void *context,const struct storage_node *parent,const char *name,uint32_t type,
                                   const struct reliefos_permissions *value,const char *target,struct storage_node *out,void **pin);
/**
 * @brief Best-effort native disk access-time update for this exact attachment.
 * @param context Borrowed live native storage instance pinned by its mount.
 * @param node Borrowed actual retained regular/directory inode in that instance.
 * @param mount_flags Effective attachment and shared-superblock flags for this access.
 * @return None. Metadata failures never replace a completed read/readdir result.
 * Readonly/noatime/nodiratime suppress I/O; relatime uses mtime/ctime or a full
 * day and strict access uses each native codec's time precision. Existing tmpfs
 * uses its own inode policy; immutable ISO records are never changed.
 * Storage execution ownership is required. A synchronous nested I/O scope
 * preserves outer admission and mutation state; no graph/module callback lock
 * spans native metadata I/O and no path is reconstructed from a display name.
 */
void storage_backend_access(void *context,const struct storage_node *node,uint64_t mount_flags);
/** @brief Report metadata of the actual retained filesystem object.
 * @param context Borrowed pinned native volume. @param path Required retained path.
 * @param out Required caller-owned stat buffer, unspecified on provider error.
 * @return Zero or negative validation/metadata errno. Cluster timestamps and
 * permissions belong to the generation-stamped held inode, including orphans;
 * no removed directory slot or displayed name is used as identity. Caller owns
 * execution; native I/O occurs without graph/module callback locks.
 */
int storage_backend_getattr(void *context,const struct vfs_path *path,struct linux_stat_abi *out);
int storage_backend_statfs(void *context,const struct storage_node *node,struct linux_statfs_abi *out);
int storage_backend_write(void *context,struct storage_node *node,uint64_t offset,
                             const void *bytes,uint32_t length,uint32_t *written);
int storage_backend_truncate(void *context,struct storage_node *node,uint64_t length);
int storage_backend_fallocate(void *context,struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length);
int storage_backend_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only);
int storage_backend_utimens(void *context,const struct storage_node *node,int64_t atime,int64_t mtime,
                               bool set_atime,bool set_mtime);
bool storage_backend_same_node(void *context,const struct storage_node *left,const struct storage_node *right);
/** @brief Remove a verified retained member through its selected native provider.
 * @param context Borrowed pinned instance. @param path Retained victim/parent.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or validation/readonly/stale/native errno. Caller authorizes;
 * execution ownership required; open backing remains pinned, no callback lock.
 */
int storage_backend_remove_node(void *context,const struct vfs_path *path,bool directory);
int storage_backend_prime_metadata(const struct vfs_path *directory);
/** @brief Rename retained operands with sidecar state reserved before mutation.
 * @param context Borrowed pinned instance. @param source Actual source member.
 * @param parent Destination directory. @param name Validated new component.
 * @param target Optional replacement. @param committed Required native commit output.
 * @return Zero or validation/readonly/native/metadata errno. Execution ownership
 * required; committed stays true on later persistence error and no callback
 * lock spans I/O. Graph operands and implementation remain owned by caller.
 */
int storage_backend_rename_node(void *context,const struct vfs_path *source,const struct vfs_path *parent,
                                   const char *name,const struct vfs_path *target,bool *committed);
/** @brief Publish a hard link through the selected native inode operation.
 * @param context Borrowed pinned instance. @param source Actual source inode.
 * @param parent Retained destination directory. @param name Validated component.
 * @return Zero or validation/readonly/unsupported/native errno. Caller authorizes;
 * execution/mutation admission required, no graph/module lock spans I/O.
 */
int storage_backend_link_node(void *context,const struct storage_node *source,
                                 const struct storage_node *parent,const char *name);
/* All codec buffers, inode pins, writer leases and storage capacity are
 * reserved before rename. Commit performs only fixed-size native I/O; finish
 * is mandatory after success, failure or a native rename that did not commit. */
struct storage_rename_metadata;
int storage_backend_prepare_rename_metadata(struct storage_volume *volume,const struct vfs_path *source,
                                               const struct vfs_path *parent,const char *name,
                                               const struct reliefos_permissions *value,
                                               struct storage_rename_metadata **out);
int storage_backend_commit_rename_metadata(struct storage_rename_metadata *change);
int storage_backend_finish_rename_metadata(struct storage_rename_metadata *change,bool committed);
/** @brief Pin a mounted backend root for the referenced VFS namespace.
 * @param volume Initialized disk/tmpfs/ISO backend, borrowed for mount lifetime.
 * @param owner Optional explicit admission owner; NULL selects the type/table owner.
 * @param out Receives one owned detached mount; NULL on failure.
 * @return Zero or negative errno, including EOPNOTSUPP until a backend has real
 * inode-lifetime support. Execution transaction required; backend I/O may wait.
 */
int storage_create_vfs_mount(struct storage_volume *volume,struct module *owner,
                               struct vfs_mount **out);
#endif

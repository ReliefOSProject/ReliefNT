#ifndef RELIEFNT_PERMISSIONS_H
#define RELIEFNT_PERMISSIONS_H

#include <reliefnt/sched.h>
#include <reliefnt/storage.h>

#define FS_ACCESS_EXEC 1U
#define FS_ACCESS_WRITE 2U
#define FS_ACCESS_READ 4U
/** @brief Apply DAC and filesystem policy to an already retained object.
 * @param task Borrowed credentials, NULL for trusted kernel capability access.
 * @param path Borrowed held path, stable under execution ownership.
 * @param access FS_ACCESS_* mask; no pathname traversal is performed.
 * @param real_ids Select real IDs/permitted root capabilities for access(2).
 * @return Zero or negative errno; uid zero alone never grants capabilities.
 * Metadata I/O may sleep without graph locks; caller keeps the path pinned.
 */
int fs_permissions_check_path(const struct task *task,const struct vfs_path *path,
                                 uint32_t access,bool real_ids);
/** @brief Change permissions on the exact retained inode after owner checks.
 * @param task Borrowed filesystem credentials, NULL for trusted kernel access.
 * @param path Held inode retaining its provider through metadata I/O.
 * @param mode New low permission/special bits; inode type is preserved.
 * @return Zero or negative errno. Honors readonly, CAP_FOWNER/CAP_FSETID and
 * group membership without resolving a displayed name or holding graph locks.
 */
int fs_permissions_chmod_path(const struct task *task,const struct vfs_path *path,uint32_t mode);
/** @brief Change ownership on a retained inode using the common DAC policy.
 * @param task Borrowed credentials, NULL for trusted kernel access.
 * @param path Held inode retaining its provider through I/O.
 * @param uid New user ID, UINT32_MAX to retain it.
 * @param gid New group ID, UINT32_MAX to retain it.
 * @return Zero or negative errno. Honors readonly, CAP_CHOWN, owner/groups and
 * removal of set-ID bits; no namespace replay or graph lock across callbacks.
 */
int fs_permissions_chown_path(const struct task *task,const struct vfs_path *path,uint32_t uid,uint32_t gid);
/** @brief Read/update synthetic inode metadata without global path lookup.
 * @param intrinsic Provider-owned intrinsic key, independent of mount location.
 * @param node Borrowed devfs/proc/sysfs node.
 * @param value Read result or new authorized metadata.
 * @param write True for a metadata update; proc/sysfs stay read-only.
 * @return Zero or negative errno. Caller serializes device metadata updates;
 * this does not authorize chmod/chown and never resolves a namespace path.
 */
int fs_virtual_permissions(const char *intrinsic,const struct storage_node *node,
                              struct reliefos_permissions *value,bool write);
/** @brief Authorize chmod on an implicit builtin virtual endpoint.
 * @param task Borrowed filesystem credentials, NULL for trusted kernel access.
 * @param intrinsic Borrowed provider key; never resolved as a pathname.
 * @param node Borrowed synthetic inode held by the endpoint admission.
 * @param mode New permission/special bits; the inode type is preserved.
 * @return Zero or negative metadata/authorization errno. Non-virtual nodes
 * return EOPNOTSUPP; no disk lookup or namespace fallback occurs. Caller owns
 * execution and endpoint lifetime; no graph/module lock spans callbacks.
 */
int fs_virtual_chmod(const struct task *task,const char *intrinsic,
                         const struct storage_node *node,uint32_t mode);
/** @brief Authorize chown on an implicit builtin virtual endpoint.
 * @param task Borrowed filesystem credentials, NULL for trusted kernel access.
 * @param intrinsic Borrowed provider key; never resolved as a pathname.
 * @param node Borrowed synthetic inode held by the endpoint admission.
 * @param uid New user ID, UINT32_MAX preserves it.
 * @param gid New group ID, UINT32_MAX preserves it.
 * @return Zero or negative metadata/authorization errno. Uses common ownership
 * and set-ID rules, without a disk lookup or namespace fallback. Caller owns
 * execution and endpoint lifetime; no graph/module lock spans callbacks.
 */
int fs_virtual_chown(const struct task *task,const char *intrinsic,
                         const struct storage_node *node,uint32_t uid,uint32_t gid);

/**
 * @brief Check filesystem or real primary and supplementary group membership.
 * @param task Borrowed task credentials, NULL for the kernel root-group default.
 * @param gid Group identity to compare.
 * @param real_ids Use the real primary gid instead of fsgid when true.
 * @return True for a matching group; a NULL task matches only gid zero.
 * @par Context
 * Execution-owned credential snapshot; no reference, allocation, user access or capability override.
 */
bool task_in_group(const struct task *task, uint32_t gid, bool real_ids);
void task_groups_retain(struct task_groups *groups);
void task_groups_release(struct task *task);
int task_groups_set(struct task *task, const uint32_t *ids, uint32_t count);

#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SYSCALL_UNIX_PATH_H
#define RELIEFNT_SYSCALL_UNIX_PATH_H
#include <reliefnt/mount.h>
struct task;
struct unix_address {char path[109];uint32_t length;struct vfs_path object;};
/** @brief Capture an AF_UNIX address and bind or resolve its actual inode.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable filesystem credentials and paths.
 * @param address User sockaddr_un bytes, snapshotted across matching retries.
 * @param length Counted address length including family, at most sockaddr_un.
 * @param create True to create a pathname socket, false to authorize its peer.
 * @param out Empty result owning one path for filesystem addresses; abstract
 * addresses retain all counted bytes including embedded NUL and own no path.
 * @return Zero or negative errno with empty output on failure. Existing bind
 * entries return EADDRINUSE. Peer lookup requires inode write permission and
 * socket type. Caller puts out->object; no graph/module lock spans callbacks.
 */
int syscall_unix_address(struct vfs_namespace *ns,struct task *task,uint64_t address,
                             uint32_t length,bool create,struct unix_address *out);
/** @brief Resolve only a captured pathname UNIX endpoint through held objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable credentials and FS references.
 * @param name Kernel-owned nonempty pathname, copied by the address adapter.
 * @param create True for bind creation, false for peer write/type checks.
 * @param out Empty output owning one inode path on success, caller must put.
 * @return Zero or negative errno. No raw user pointer or diagnostic name is
 * retained and no graph/module lock spans provider metadata callbacks.
 */
int syscall_unix_path(struct vfs_namespace *ns,struct task *task,const char *name,
                          bool create,struct vfs_path *out);
#endif

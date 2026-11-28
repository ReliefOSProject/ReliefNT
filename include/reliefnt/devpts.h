/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEVPTS_H
#define RELIEFNT_DEVPTS_H
#include <reliefnt/mount.h>

/** @brief Create an independent devpts instance under execution ownership.
 * @param source Optional diagnostic mount label.
 * @param flags Supported mount policy flags.
 * @param options Optional uid/gid/mode/ptmxmode/max/newinstance mount data.
 * @param uid Root/ptmx owner filesystem UID.
 * @param gid Root/ptmx owner filesystem GID.
 * @param out Owned detached mount, empty on failure.
 * @return Zero or negative option/allocation errno, with balanced rollback. */
int devpts_create_vfs_mount(const char *source, uint64_t flags,
                            const char *options, uint32_t uid, uint32_t gid,
                            struct vfs_mount **out);

/** @brief Resolve the actual devpts multiplexor for a retained device inode.
 * @param device Borrowed devpts ptmx or legacy 5:2 device path.
 * @param root Borrowed caller root constraining sibling traversal.
 * @param out Independently retained devpts ptmx path, empty on failure.
 * @return Zero or negative ENODEV/lookup errno. A legacy inode selects the
 * sibling pts/ptmx through real mount attachments under execution ownership. */
int devpts_resolve_ptmx(const struct vfs_path *device,
                        const struct vfs_path *root, struct vfs_path *out);

/** @brief Allocate and publish a PTY in the exact retained ptmx instance.
 * @param ptmx Borrowed authorized devpts multiplexor path.
 * @param owner_pid Creating task's internal ID.
 * @param uid Default slave owner filesystem UID when uid= is absent.
 * @param gid Default slave owner filesystem GID when gid= is absent.
 * @param out Internal session ID, zero on failure.
 * @return Zero or negative errno. Caller must install a master reference or
 * destroy the new session on FD failure. Requires execution ownership. */
int devpts_new_pty(const struct vfs_path *ptmx, uint32_t owner_pid,
                    uint32_t uid, uint32_t gid, uint32_t *out);

/** @brief Resolve a held slave inode to its original live session.
 * @param path Borrowed actual devpts slave path.
 * @param out Internal session ID, zero on failure.
 * @return Zero or negative EIO/ENODEV. Checks instance, inode and generation
 * under execution ownership, with no global device-number lookup. */
int devpts_slave_pty(const struct vfs_path *path, uint32_t *out);
#endif

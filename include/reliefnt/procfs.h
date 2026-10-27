/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PROCFS_H
#define RELIEFNT_PROCFS_H
#include <reliefnt/mount.h>

/** @brief Create an independently owned proc filesystem mount.
 * @param out Empty output receiving one detached mount, empty on failure.
 * @return Zero or negative errno, with balanced key/PID/context rollback.
 * Task context under execution ownership. Inodes retain PID identities; names
 * inside proc remain independent of namespace attachment, overmount or MS_MOVE.
 */
int proc_create_vfs_mount(struct vfs_mount **out);
#endif

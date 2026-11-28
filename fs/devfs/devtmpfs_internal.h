/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEVTMPFS_INTERNAL_H
#define RELIEFNT_DEVTMPFS_INTERNAL_H
#include <reliefnt/tmpfs.h>

struct devtmpfs_node {
    struct devtmpfs_node *next;
    uint64_t device;
    uint32_t mode, ino;
    bool seen;
    char target[48];
    char path[RELIEFOS_FS_PATH_LEN];
};
/** @brief Snapshot the intrinsic device hierarchy before changing any nodes.
 * @param out Owned preorder list, empty on failure. Execution ownership required.
 * @return Zero or enumeration/allocation errno. No namespace paths are followed.
 */
int devtmpfs_snapshot(struct devtmpfs_node **out);
/** @brief Release a snapshot and its optional retained tmpfs inode references.
 * @param fs Owning filesystem, NULL for an unbound snapshot. @param nodes List.
 */
void devtmpfs_nodes_free(struct tmpfs_super *fs,struct devtmpfs_node *nodes);
/** @brief Reconcile one complete inventory with retained device inode identities.
 * @param fs Serialized backing filesystem. @param managed Persistent owned list.
 * @param snapshot Complete fresh inventory, consumed on all paths.
 * @return Zero or allocation/provider errno. User replacements and metadata
 * survive unrelated device changes. Failed snapshots never reach this helper.
 */
int devtmpfs_reconcile(struct tmpfs_super *fs,struct devtmpfs_node **managed,
                       struct devtmpfs_node *snapshot);
#endif

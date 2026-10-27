/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_CLUSTER_TIME_H
#define RELIEFNT_FS_CLUSTER_TIME_H
#include <stdint.h>
struct storage_node;
struct storage_inode_ref;
struct storage_inode_times {
    int64_t atime,mtime,ctime;
    uint32_t atime_nsec,mtime_nsec,ctime_nsec;
};
/** @brief Populate a retained cluster inode's native timestamps once.
 * @param reference Borrowed cache object, stable under execution ownership.
 * @return Zero or negative selection/metadata errno. Unlinked objects must
 * already own their timestamps; no old directory slot is consulted after unlink.
 * May read native metadata, without graph/module locks or ownership transfer.
 */
int storage_cluster_load_times(struct storage_inode_ref *reference);
/** @brief Copy timestamps from the actual generation-stamped cluster inode.
 * @param node Required retained native node. @param out Required output, zero on error.
 * @return Zero or negative retention/metadata errno. Execution ownership required;
 * temporary inode ownership balances on every path and no display name is used.
 */
int storage_cluster_node_times(const struct storage_node *node,struct storage_inode_times *out);
/** @brief Mark one actual FAT/exFAT inode access at its native timestamp precision.
 * @param node Required retained node. @param seconds Current UTC Unix seconds.
 * @return Zero or negative selection/metadata errno. Execution ownership and
 * synchronous storage required; admitted path keeps code pinned. Linked files
 * update their current entry, while roots/orphans update only their own cache.
 * No graph/module lock spans I/O; failure never publishes a new cached timestamp.
 */
int storage_cluster_touch_node(const struct storage_node *node,int64_t seconds);
#endif

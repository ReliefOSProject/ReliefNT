/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../cluster.h"
#include "../dos_time.h"
#include <linux/errno.h>

/** @brief Populate a retained cluster inode's native timestamps once.
 * @param reference Borrowed cache object, stable under execution ownership.
 * @return Zero or negative selection/metadata errno. Unlinked objects must
 * already own their timestamps; no old directory slot is consulted after unlink.
 * May read native metadata, without graph/module locks or ownership transfer.
 */
int storage_cluster_load_times(struct storage_inode_ref *reference)
{
    if(!reference || !(reference->node.flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)))return -EINVAL;
    if(reference->times_valid)return 0;
    if(reference->unlinked)return -ESTALE;
    struct storage_volume *old=NULL;int rc=storage_select_node_volume(&reference->node,&old);
    struct storage_inode_times times={0};
    if(!rc) {
        const struct storage_cluster_ops *ops=g_storage.fs_ops ? g_storage.fs_ops->cluster : NULL;
        rc=ops && ops->times ? ops->times(&g_storage,&reference->node,&times) : -EOPNOTSUPP;
    }
    storage_restore_volume(old);
    if(!rc){reference->times=times;reference->times_valid=true;}
    return rc;
}
/** @brief Copy timestamps from the actual generation-stamped cluster inode.
 * @param node Required retained native node. @param out Required output, zero on error.
 * @return Zero or negative retention/metadata errno. Execution ownership required;
 * temporary inode ownership balances on every path and no display name is used.
 */
int storage_cluster_node_times(const struct storage_node *node,struct storage_inode_times *out)
{
    if(!out)return -EINVAL;
    *out=(struct storage_inode_times){0};
    if(!node || !(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)))return -EINVAL;
    struct storage_inode_ref *reference=NULL;int rc=storage_inode_get(node,&reference);
    if(!rc && !reference)rc=-ESTALE;
    if(!rc)rc=storage_cluster_load_times(reference);
    if(!rc)*out=reference->times;
    int close=storage_inode_put(reference);return rc ? rc : close;
}
/** @brief Mark one actual FAT/exFAT inode access at its native timestamp precision.
 * @param node Required retained node. @param seconds Current UTC Unix seconds.
 * @return Zero or negative selection/metadata errno. Execution ownership and
 * synchronous storage required; admitted path keeps code pinned. Linked files
 * update their current entry, while roots/orphans update only their own cache.
 * No graph/module lock spans I/O; failure never publishes a new cached timestamp.
 */
int storage_cluster_touch_node(const struct storage_node *node,int64_t seconds)
{
    if(!node || !(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)))return -EINVAL;
    struct storage_inode_ref *reference=NULL;int rc=storage_inode_get(node,&reference);
    if(!rc && !reference)rc=-ESTALE;
    struct storage_volume *old=NULL;
    if(!rc)rc=storage_select_node_volume(&reference->node,&old);
    int64_t actual=0;
    if(!rc && reference->unlinked) {
        struct storage_dos_time encoded;uint32_t fraction;
        rc=storage_dos_encode(seconds,0,&encoded);
        if(!rc)rc=storage_dos_decode(encoded.date,reference->node.flags&STORAGE_NODE_FLAG_EXFAT ? encoded.clock : 0,
                                    0,&actual,&fraction);
        if(!rc && reference->times.atime!=actual)storage_begin_mutation();
    } else if(!rc) {
        const struct storage_cluster_ops *ops=g_storage.fs_ops ? g_storage.fs_ops->cluster : NULL;
        rc=ops && ops->access ? ops->access(&g_storage,&reference->node,seconds,&actual) : -EOPNOTSUPP;
    }
    storage_restore_volume(old);
    if(!rc){reference->times.atime=actual;reference->times.atime_nsec=0;}
    int close=storage_inode_put(reference);return rc ? rc : close;
}

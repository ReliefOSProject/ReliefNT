/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Validate/bound one inode read before entering its selected provider.
 * @param node Borrowed retained native inode. @param offset Byte offset.
 * @param buf Required validated destination. @param len Available bytes.
 * @param out_read Optional actual-count output, zeroed on entry.
 * @param cursor Optional caller hint. @param policy Optional access-local flags;
 * NULL retains the instance's defaults for internal direct-node consumers.
 * @return Zero or negative validation/provider errno. Execution ownership
 * required, native selection always restored, no provider lock across I/O.
 */
static int storage_read_node_cursor_unlocked(const struct storage_node *node,uint64_t offset,
                                               void *buf,uint32_t len,uint32_t *out_read,
                                               struct storage_read_cursor *cursor,const uint64_t *policy)
{
    if(cursor && (!cursor->valid || cursor->offset!=offset || cursor->cluster<2))cursor->valid=0;
    if(out_read)*out_read=0;
    if(!node || !buf)return -EINVAL;
    struct storage_node refreshed=*node;
    if(node->flags&(STORAGE_NODE_FLAG_EXT2|STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
        int rc=storage_inode_refresh(&refreshed);if(rc)return rc;node=&refreshed;
    }
    if(node->type!=RELIEFOS_FS_TYPE_FILE)return -EISDIR;
    if(offset>=node->size || !len){if(cursor)cursor->valid=0;return 0;}
    struct storage_volume *previous=NULL;
    int rc=storage_select_node_volume(node,&previous);if(rc)return rc;
    if(len>node->size-offset)len=(uint32_t)(node->size-offset);
    const struct storage_filesystem_ops *ops=g_storage.fs_ops;
    rc=ops && ops->read ? ops->read(&g_storage,node,offset,buf,len,out_read,cursor,
        policy ? *policy : g_storage.mount_flags) : -EOPNOTSUPP;
    storage_restore_volume(previous);return rc;
}

/** @brief Read a retained inode using its native instance's internal policy.
 * @param node Borrowed actual file. @param offset Byte position.
 * @param buf Required validated destination. @param len Available byte count.
 * @param out_read Optional actual-count output.
 * @return Zero or negative native/input errno. Recursively enters execution;
 * provider code and inode remain pinned by caller, no path reconstruction.
 */
int storage_read_node(const struct storage_node *node,uint64_t offset,
                        void *buf,uint32_t len,uint32_t *out_read)
{ return storage_read_node_cursor(node,offset,buf,len,out_read,NULL); }

/** @brief Read an inode with a caller hint and the instance's internal policy.
 * @param node Borrowed actual file. @param offset Byte position.
 * @param buf Required destination. @param len Available bytes.
 * @param out_read Optional actual-count output. @param cursor Optional hint.
 * @return Zero or negative errno. Recursively enters execution; caller pins
 * provider/inode, native selection restored and no graph/module lock spans I/O.
 */
int storage_read_node_cursor(const struct storage_node *node,uint64_t offset,
                               void *buf,uint32_t len,uint32_t *out_read,struct storage_read_cursor *cursor)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=storage_read_node_cursor_unlocked(node,offset,buf,len,out_read,cursor,NULL);
    kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Read a retained inode with the actual calling attachment's policy.
 * @param node Borrowed actual file. @param offset Byte position.
 * @param bytes Required destination. @param capacity Available bytes.
 * @param length Optional actual-count output. @param cursor Optional hint.
 * @param mount_flags Effective attachment/shared flags, not stored globally.
 * @return Zero or negative errno. Recursively enters execution; caller pins
 * provider/inode, native selection restored and no graph/module lock spans I/O.
 */
int storage_read_node_policy(const struct storage_node *node,uint64_t offset,void *bytes,
                               uint32_t capacity,uint32_t *length,struct storage_read_cursor *cursor,
                               uint64_t mount_flags)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=storage_read_node_cursor_unlocked(node,offset,bytes,capacity,length,cursor,&mount_flags);
    kernel_execution_unlock_irqrestore(flags);return rc;
}

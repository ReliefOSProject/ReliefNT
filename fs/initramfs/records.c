/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Find a current-archive hardlink by its complete device/inode tuple.
 * @param context Exclusive state. @param entry Borrowed wire record.
 * @return Borrowed ledger member or NULL; pure, no allocation/locks/I/O.
 */
struct unpack_link *initramfs_link_find(struct unpack_context *context,const struct initramfs_entry *entry)
{
    for(struct unpack_link *link=context->links;link;link=link->next)
        if(link->major==entry->device_major && link->minor==entry->device_minor && link->inode==entry->inode)
            return link;
    return NULL;
}
/** @brief Record and hold the first actual inode for a nondirectory tuple.
 * @param context Exclusive state. @param entry Borrowed wire tuple and type.
 * @param node Actual inode metadata whose lifetime must span later members.
 * @return Zero or ENOMEM/native inode errno; failure publishes no ledger/hold.
 * Caller owns private fs and execution, no pathname lookup/locks/disk I/O.
 */
int initramfs_link_add(struct unpack_context *context,const struct initramfs_entry *entry,const struct storage_node *node)
{
    struct unpack_link *link=kernel_malloc(sizeof(*link));if(!link)return -ENOMEM;
    int rc=tmpfs_hold(context->fs,node->first_cluster);if(rc){kernel_free(link);return rc;}
    *link=(struct unpack_link){context->links,entry->device_major,entry->device_minor,
        entry->inode,entry->mode&LINUX_S_IFMT,node->first_cluster};
    context->links=link;return 0;
}
/** @brief Reset archive tuple state without retaining deleted-inode backing.
 * @param context Exclusive construction state, fs must remain alive.
 * @return None. Drops every held inode and frees every tuple; no locks/I/O.
 */
void initramfs_links_clear(struct unpack_context *context)
{
    while(context->links) {
        struct unpack_link *link=context->links;context->links=link->next;
        tmpfs_drop(context->fs,link->node);kernel_free(link);
    }
}
/** @brief Remember a directory's final archive mtime independent of child order.
 * @param context Exclusive state. @param node Actual directory inode.
 * @param mtime Requested unsigned archive seconds.
 * @return Zero or ENOMEM/native inode errno, with no hold on failure. Repeated
 * records update in place; no namespace lookup, locks or disk I/O occurs.
 */
int initramfs_directory_time(struct unpack_context *context,uint32_t node,uint32_t mtime)
{
    for(struct unpack_time *time=context->times;time;time=time->next)
        if(time->node==node){time->mtime=mtime;return 0;}
    struct unpack_time *time=kernel_malloc(sizeof(*time));if(!time)return -ENOMEM;
    int rc=tmpfs_hold(context->fs,node);if(rc){kernel_free(time);return rc;}
    *time=(struct unpack_time){context->times,node,mtime};context->times=time;return 0;
}
/** @brief Finish/discard deferred times and release their independent inode holds.
 * @param context Exclusive state with live fs. @param apply Store times if true.
 * @return First native time errno, otherwise zero; every hold/ledger retires
 * even after failure. No path reconstruction, module locks or disk I/O.
 */
int initramfs_times_finish(struct unpack_context *context,bool apply)
{
    int result=0;
    while(context->times) {
        struct unpack_time *time=context->times;context->times=time->next;
        if(apply) {
            int rc=tmpfs_utimens(context->fs,time->node,time->mtime,time->mtime,true,true);
            if(!result)result=rc;
        }
        tmpfs_drop(context->fs,time->node);kernel_free(time);
    }
    return result;
}

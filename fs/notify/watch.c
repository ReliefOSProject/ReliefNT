/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define FSNOTIFY_STORED_FLAGS (IN_EXCL_UNLINK|IN_ONESHOT)

static uint64_t watch_total;

/** @brief Check the global watch count without walking open instances.
 * @return True if at least one watch is live. Execution writer is required;
 * the scalar query acquires no inode, provider, mount, or path reference.
 */
bool fsnotify_has_watches(void)
{return watch_total!=0;}

/** @brief Compare domains before invoking the provider's live identity callback.
 * @param a First borrowed weak key, or NULL.
 * @param b Second borrowed weak key, or NULL.
 * @return True when the live provider identifies the same inode in its domain.
 * The caller holds the execution writer and keeps callback code/context live;
 * neither key retains a provider, mount, path, or inode reference.
 */
bool fsnotify_key_same(const struct fsnotify_key *a,const struct fsnotify_key *b)
{
    return a && b && a->ops && a->ops==b->ops && a->context==b->context &&
        a->ops->same_node && a->ops->same_node(a->context,&a->node,&b->node);
}

/** @brief Find an unused positive watch descriptor, wrapping without collision.
 * @param instance Borrowed live instance whose descriptor cursor is updated.
 * @return Positive unused descriptor or -ENOSPC. Execution writer is required;
 * no watch, inode, or provider ownership is transferred.
 */
static int fsnotify_allocate_wd(struct fsnotify_instance *instance)
{
    int first=instance->next_wd;
    do {
        int candidate=instance->next_wd;
        instance->next_wd=candidate==INT32_MAX ? 1 : candidate+1;
        struct fsnotify_watch *watch=instance->watches;
        while(watch && watch->wd!=candidate)watch=watch->next;
        if(!watch)return candidate;
    } while(instance->next_wd!=first);
    return -ENOSPC;
}

/** @brief Attach or update a watch without retaining a backend or inode.
 * @param instance Borrowed live instance charged for a new watch.
 * @param key Borrowed weak inode key with live provider code and context.
 * @param mask Requested Linux event bits and watch flags.
 * @return Positive descriptor or negative EINVAL, EEXIST, ENOSPC, or ENOMEM.
 * Execution writer is required; callers retire watches before the weak key's
 * backend or inode identity expires. Updates do not create a new UID charge.
 */
int fsnotify_add_watch(struct fsnotify_instance *instance,const struct fsnotify_key *key,uint32_t mask)
{
    if(!instance || !key || !key->ops || !key->ops->same_node ||
       (mask & ~FSNOTIFY_SUPPORTED_MASK) || !mask ||
       ((mask & (IN_MASK_ADD|IN_MASK_CREATE))==(IN_MASK_ADD|IN_MASK_CREATE)))return -EINVAL;
    uint32_t requested=mask & (IN_ALL_EVENTS|FSNOTIFY_STORED_FLAGS);
    for(struct fsnotify_watch *watch=instance->watches;watch;watch=watch->next) {
        if(!fsnotify_key_same(&watch->key,key))continue;
        if(mask & IN_MASK_CREATE)return -EEXIST;
        if(mask & IN_MASK_ADD)watch->mask|=requested;
        else watch->mask=requested;
        return watch->wd;
    }
    int rc=fsnotify_account_watch_acquire(instance->account);if(rc)return rc;
    struct fsnotify_watch *watch=kernel_malloc(sizeof(*watch));
    if(!watch){fsnotify_account_watch_release(instance->account);return -ENOMEM;}
    int wd=fsnotify_allocate_wd(instance);
    if(wd<0){kernel_free(watch);fsnotify_account_watch_release(instance->account);return wd;}
    *watch=(struct fsnotify_watch){.next=instance->watches,.key=*key,.mask=requested,.wd=wd};
    instance->watches=watch;++watch_total;return wd;
}

/** @brief Unlink a watch, optionally reporting the standard ignored event.
 * @param instance Borrowed instance owning the linked watch.
 * @param link Required pointer to the live watch's list link.
 * @param ignored True to enqueue IN_IGNORED before freeing the watch.
 * Execution writer is required; this consumes the watch and its UID charge,
 * but its weak key owns no inode or provider reference.
 */
void fsnotify_watch_remove(struct fsnotify_instance *instance,struct fsnotify_watch **link,bool ignored)
{
    struct fsnotify_watch *watch=*link;
    *link=watch->next;
    if(ignored)fsnotify_queue_event(instance,watch->wd,IN_IGNORED,0,NULL);
    fsnotify_account_watch_release(instance->account);
    --watch_total;kernel_free(watch);
}

/** @brief Remove a descriptor and append IN_IGNORED.
 * @param instance Borrowed live instance, or NULL.
 * @param wd Watch descriptor to retire.
 * @return Zero or -EINVAL. Execution writer is required; the retired watch's
 * UID charge is released and its weak identity owns no backend reference.
 */
int fsnotify_remove_watch(struct fsnotify_instance *instance,int wd)
{
    if(!instance || wd<=0)return -EINVAL;
    struct fsnotify_watch **link=&instance->watches;
    while(*link && (*link)->wd!=wd)link=&(*link)->next;
    if(!*link)return -EINVAL;
    fsnotify_watch_remove(instance,link,true);return 0;
}

/** @brief Fan one successful operation out to matching inode watches.
 * @param key Borrowed weak inode identity with a live provider, or NULL.
 * @param mask Linux event bits and optional IN_ISDIR.
 * @param cookie Rename cookie or zero.
 * @param name Borrowed optional basename, copied into each queued event.
 * @param unlinked True for a child operation after unlink, for EXCL_UNLINK.
 * Execution writer is required and keeps the provider code and context live.
 * Queue allocation failure produces overflow rather than an operation error.
 */
void fsnotify_emit(const struct fsnotify_key *key,uint32_t mask,uint32_t cookie,
                   const char *name,bool unlinked)
{
    if(!key || !(mask & IN_ALL_EVENTS))return;
    for(struct fsnotify_instance *instance=fsnotify_instances;instance;instance=instance->next) {
        struct fsnotify_watch **link=&instance->watches;
        while(*link) {
            struct fsnotify_watch *watch=*link;
            uint32_t selected=watch->mask & mask & IN_ALL_EVENTS;
            if(!selected || (unlinked && (watch->mask & IN_EXCL_UNLINK)) ||
               !fsnotify_key_same(&watch->key,key)){link=&watch->next;continue;}
            fsnotify_queue_event(instance,watch->wd,selected|(mask & IN_ISDIR),cookie,name);
            if(watch->mask & IN_ONESHOT)fsnotify_watch_remove(instance,link,true);
            else link=&watch->next;
        }
    }
}

/** @brief Retire all watches on a terminal inode after DELETE_SELF delivery.
 * @param key Borrowed terminal weak inode identity, or NULL.
 * Execution writer is required; the provider and identity must still be live
 * until this call finishes. Matching watches relinquish their UID charges.
 */
void fsnotify_delete(const struct fsnotify_key *key)
{
    if(!key)return;
    for(struct fsnotify_instance *instance=fsnotify_instances;instance;instance=instance->next) {
        struct fsnotify_watch **link=&instance->watches;
        while(*link) {
            struct fsnotify_watch *watch=*link;
            if(!fsnotify_key_same(&watch->key,key)){link=&watch->next;continue;}
            if(watch->mask & IN_DELETE_SELF)fsnotify_queue_event(instance,watch->wd,IN_DELETE_SELF,0,NULL);
            fsnotify_watch_remove(instance,link,true);
        }
    }
}

/** @brief Retire all watches for a backend instance before its code/context dies.
 * @param ops Borrowed live backend operations table, or NULL.
 * @param context Borrowed backend domain identity.
 * Execution writer is required. UNMOUNT and IGNORED are queued before the
 * weak watches and their UID charges are retired; no backend pin is held.
 */
void fsnotify_unmount(const struct vfs_backend_ops *ops,void *context)
{
    if(!ops)return;
    for(struct fsnotify_instance *instance=fsnotify_instances;instance;instance=instance->next) {
        struct fsnotify_watch **link=&instance->watches;
        while(*link) {
            struct fsnotify_watch *watch=*link;
            if(watch->key.ops!=ops || watch->key.context!=context){link=&watch->next;continue;}
            fsnotify_queue_event(instance,watch->wd,IN_UNMOUNT,0,NULL);
            fsnotify_watch_remove(instance,link,true);
        }
    }
}

/** @brief Keep watched native inode generations resident without pinning mounts.
 * @param node Borrowed FAT/exFAT node with a stable generation, or NULL.
 * @return True if a weak watch matches its filesystem, volume, mount, and inode
 * generation. Execution writer is required; no inode or mount pin is acquired.
 */
bool fsnotify_storage_node_watched(const struct storage_node *node)
{
    const uint32_t cluster_flags=STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT;
    if(!node || !node->inode_generation || !(node->flags & cluster_flags))return false;
    for(struct fsnotify_instance *instance=fsnotify_instances;instance;instance=instance->next)
        for(struct fsnotify_watch *watch=instance->watches;watch;watch=watch->next) {
            const struct storage_node *other=&watch->key.node;
            if((other->flags & cluster_flags)==(node->flags & cluster_flags) &&
               other->volume_id==node->volume_id && other->mount_generation==node->mount_generation &&
               other->inode_generation==node->inode_generation)return true;
        }
    return false;
}

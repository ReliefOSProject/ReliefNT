#include <reliefnt/export.h>
#include "../../block/core/storage_internal.h"
#include <reliefnt/page_cache.h>
#include <reliefnt/permissions.h>

/* Reference backing objects independently of directory links. The storage
 * execution lock protects this list, lookup, unlink and inode allocation. */
static struct storage_inode_ref *storage_inode_refs;
static uint32_t storage_cluster_generation;

static bool storage_inode_clustered(const struct storage_node *node)
{return node->flags & (STORAGE_NODE_FLAG_FAT32 | STORAGE_NODE_FLAG_EXFAT);}

static struct storage_inode_ref *storage_inode_for_node(const struct storage_node *node)
{
    for(struct storage_inode_ref *ref=storage_inode_refs;ref;ref=ref->next) {
        if(ref->node.volume_id!=node->volume_id)continue;
        if(storage_inode_clustered(node)) {
            if(!storage_inode_clustered(&ref->node))continue;
            if(node->inode_generation) {
                if(node->inode_generation==ref->node.inode_generation)return ref;
            } else if(!ref->unlinked && node->entry_id==ref->node.entry_id &&
                      node->type==ref->node.type &&
                      (!(node->flags & STORAGE_NODE_FLAG_ROOT) || node->first_cluster==ref->node.first_cluster))return ref;
        } else if(ref->node.first_cluster==node->first_cluster)return ref;
    }
    return NULL;
}

bool storage_inode_is_unlinked(const struct storage_node *node)
{
    struct storage_inode_ref *ref=storage_inode_for_node(node);
    return ref && ref->unlinked;
}

/** @brief Intern a FAT/exFAT directory record with stable per-mount inode identity.
 * @param node Borrowed metadata, updated with a unique cache generation.
 * @return Zero or negative errno. Execution transaction required. Unused cache
 * entries may be reclaimed; held entries have no fixed capacity cutoff.
 */
int storage_inode_cache_node(struct storage_node *node)
{
    if(!node || !storage_inode_clustered(node))return -22;
    struct storage_node slot=*node;slot.inode_generation=0;
    struct storage_inode_ref *ref=storage_inode_for_node(&slot);
    if(ref){*node=ref->node;return 0;}
    /* This is only an unused metadata cache budget, never a held-inode cap. */
    size_t unused=0;struct storage_inode_ref **victim=NULL;
    for(struct storage_inode_ref **link=&storage_inode_refs;*link;link=&(*link)->next)
        if(storage_inode_clustered(&(*link)->node) && !(*link)->references && !(*link)->unlinked) {
            ++unused;if(!victim)victim=link;
        }
    if(unused>=128 && victim) {
        ref=*victim;*victim=ref->next;kernel_free(ref);
    }
    ref=kernel_malloc(sizeof(*ref));if(!ref)return -12;
    bool collision;
    do {
        if(!++storage_cluster_generation)++storage_cluster_generation;
        collision=false;
        for(struct storage_inode_ref *held=storage_inode_refs;held;held=held->next)
            if(storage_inode_clustered(&held->node) &&
               held->node.inode_generation==storage_cluster_generation){collision=true;break;}
    } while(collision);
    node->inode_generation=storage_cluster_generation;
    *ref=(struct storage_inode_ref){.node=*node,.next=storage_inode_refs};
    storage_inode_refs=ref;return 0;
}

/** @brief Publish committed cluster/size changes to every held inode alias.
 * @param before Original identity, borrowed.
 * @param after Committed backing metadata, borrowed; identity stays unchanged.
 */
void storage_inode_note_data(const struct storage_node *before,const struct storage_node *after)
{
    struct storage_inode_ref *ref=storage_inode_for_node(before);if(!ref)return;
    ref->node.first_cluster=after->first_cluster;ref->node.size=after->size;
    ref->node.flags=(ref->node.flags & ~STORAGE_NODE_FLAG_EXFAT_NOFAT) |
        (after->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT);
}

/** @brief Transfer a renamed FAT/exFAT inode to its committed directory slot.
 * @param before Original stable identity, borrowed.
 * @param after New directory-record location and metadata, borrowed.
 */
void storage_inode_note_move(const struct storage_node *before,const struct storage_node *after)
{
    struct storage_inode_ref *ref=storage_inode_for_node(before);if(!ref)return;
    storage_inode_note_data(before,after);ref->node.entry_id=after->entry_id;
}

/** @brief Disconnect a committed directory removal while deferring held chains.
 * @param node Removed identity, borrowed under execution ownership.
 * @return True if final inode put owns chain destruction; false if caller does.
 */
bool storage_inode_unlinked(const struct storage_node *node)
{
    struct storage_inode_ref *ref=storage_inode_for_node(node);if(!ref)return false;
    if(ref->references){ref->unlinked=true;return true;}
    struct storage_inode_ref **link=&storage_inode_refs;
    while(*link && *link!=ref)link=&(*link)->next;
    if(*link)*link=ref->next;kernel_free(ref);return false;
}

/** @brief Discard unused cluster inode cache records before volume reuse.
 * @param volume Backend slot, under execution ownership.
 * @return Zero or EBUSY if a held/failed-retirement inode prevents reuse.
 */
int storage_inode_purge_volume(uint32_t volume)
{
    for(struct storage_inode_ref *ref=storage_inode_refs;ref;ref=ref->next)
        if(ref->node.volume_id==volume && (ref->references || ref->unlinked))return -16;
    struct storage_inode_ref **link=&storage_inode_refs;
    while(*link) {
        struct storage_inode_ref *ref=*link;
        if(ref->node.volume_id!=volume){link=&ref->next;continue;}
        *link=ref->next;kernel_free(ref);
    }
    return 0;
}

int storage_inode_mutation_allowed(const struct storage_node *node)
{
    struct storage_inode_ref *ref=storage_inode_for_node(node);
    return ref && ref->write_count<0 ? -26 : 0;
}

/**
 * @brief Borrow a held native inode record by volume and backing inode number.
 * @param volume Live native volume slot.
 * @param inode Backing inode or first-cluster number to match.
 * @return Borrowed matching record or NULL; no reference is acquired.
 * @par Context
 * Storage execution ownership required. Keep that ownership through use or acquire an independent inode lease; lookup does not validate a mount generation.
 */
struct storage_inode_ref *storage_inode_find(uint32_t volume, uint32_t inode)
{
    for (struct storage_inode_ref *ref = storage_inode_refs; ref; ref = ref->next)
        if (ref->node.volume_id == volume && ref->node.first_cluster == inode) return ref;
    return NULL;
}

bool storage_inode_volume_busy(uint32_t volume)
{
    for (struct storage_inode_ref *ref = storage_inode_refs; ref; ref = ref->next)
        if (ref->node.volume_id == volume && (ref->references || ref->unlinked)) return true;
    return false;
}

bool storage_inode_volume_other_users(uint32_t volume,const struct storage_inode_ref *root)
{
    for(const struct storage_inode_ref *ref=storage_inode_refs;ref;ref=ref->next)
        if(ref->node.volume_id==volume &&
           (ref->references>(ref==root ? 1u : 0u) || ref->unlinked))return true;
    return false;
}

bool storage_inode_volume_writers(uint32_t volume)
{
    for(const struct storage_inode_ref *ref=storage_inode_refs;ref;ref=ref->next)
        if(ref->node.volume_id==volume && ref->write_count>0)return true;
    return false;
}

/** @brief Acquire an actual inode lease and its resident cluster timestamps.
 * @param node Required native identity; a cluster generation must remain current.
 * @param out Required owned reference output, cleared before validation.
 * @return Zero, including NULL for unsupported synthetic nodes, or negative
 * allocation/stale/native metadata/I/O errno. Serializes under execution
 * ownership. Cluster metadata is captured before unlink can reuse the record;
 * each returned reference requires storage_inode_put. No module/graph callback
 * lock spans native I/O; shared inode/cache ownership outlives directory names.
 */
int storage_inode_get(const struct storage_node *node, struct storage_inode_ref **out)
{
    if (!node || !out) return -22;
    *out = NULL;
    /* ISO media is immutable; an extent identifies its backing object while
     * held references keep the volume mounted. Exclude virtual/device nodes
     * whose volume field may also happen to identify the optical root. */
    bool optical = node->volume_id < STORAGE_MAX_VOLUMES &&
        g_volumes[node->volume_id].ready &&
        g_volumes[node->volume_id].filesystem == STORAGE_FILESYSTEM_ISO9660 &&
        !(node->flags & ~STORAGE_NODE_FLAG_ROOT) &&
        (node->type == RELIEFOS_FS_TYPE_FILE || node->type == RELIEFOS_FS_TYPE_DIR);
    bool clustered=storage_inode_clustered(node);
    if (!(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS)) && !optical && !clustered) return 0;
    if ((node->flags & STORAGE_NODE_FLAG_EXT_FAMILY) && node->mount_generation &&
        (node->volume_id >= STORAGE_MAX_VOLUMES ||
         node->mount_generation != g_volumes[node->volume_id].mount_generation)) return -116;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    struct storage_node stamped;
    if(clustered) {
        if(node->inode_generation && !storage_inode_for_node(node)) {
            kernel_execution_unlock_irqrestore(flags);return -116;
        }
        stamped=*node;int rc=node->inode_generation ? 0 : storage_inode_cache_node(&stamped);
        if(rc){kernel_execution_unlock_irqrestore(flags);return rc;}
        node=&stamped;
    }
    struct storage_inode_ref *ref = storage_inode_for_node(node);
    if (optical && ref && ref->node.type != node->type) {
        for (ref = storage_inode_refs; ref; ref = ref->next)
            if (ref->node.volume_id == node->volume_id &&
                ref->node.first_cluster == node->first_cluster && ref->node.type == node->type) break;
    }
    if (!ref) {
        ref=kernel_malloc(sizeof(*ref));
        if(!ref){kernel_execution_unlock_irqrestore(flags);return -ENOMEM;}
        struct storage_volume *previous=NULL;int ret=storage_select_node_volume(node,&previous);
        if(!ret) {
            const struct storage_filesystem_ops *ops=g_storage.fs_ops;
            ret=!ops ? -EOPNOTSUPP : ops->hold_inode ? ops->hold_inode(&g_storage,node) : 0;
        }
        storage_restore_volume(previous);
        if(ret){kernel_free(ref);kernel_execution_unlock_irqrestore(flags);return ret;}
        *ref=(struct storage_inode_ref){.node=*node,.read_only=optical,.next=storage_inode_refs};
        storage_inode_refs=ref;
    }
    if(clustered) {
        int rc=storage_cluster_load_times(ref);
        if(rc){kernel_execution_unlock_irqrestore(flags);return rc;}
    }
    __atomic_add_fetch(&ref->references, 1, __ATOMIC_RELAXED);
    *out = ref;
    kernel_execution_unlock_irqrestore(flags);
    return 0;
}

void storage_inode_retain(struct storage_inode_ref *reference)
{
    if (!reference) return;
    __atomic_add_fetch(&reference->references, 1, __ATOMIC_RELAXED);
}

/** @brief Consume a core inode reference and retire native backing at final put.
 * @param reference Owned reference, nullable; clustered linked metadata may cache at zero.
 * @return Zero or native final-cleanup errno; failed cleanup retains metadata
 * and instance/code lifetime for retry. Recursively enters execution; final
 * native I/O is synchronous and no graph/module lock spans callbacks.
 */
int storage_inode_put(struct storage_inode_ref *reference)
{
    if (!reference) return 0;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    if (reference->references && __atomic_sub_fetch(&reference->references, 1, __ATOMIC_ACQ_REL)) {
        kernel_execution_unlock_irqrestore(flags);
        return 0;
    }
    if(storage_inode_clustered(&reference->node) && !reference->unlinked) {
        kernel_execution_unlock_irqrestore(flags);return 0;
    }
    struct storage_volume *previous = NULL;
    int ret=0;bool saved_async=storage_io_async_context;
    if((reference->node.flags&STORAGE_NODE_FLAG_TMPFS) || reference->unlinked) {
        /* Final inode cleanup cannot be replayed after ownership is consumed. */
        if(reference->unlinked)storage_io_async_context=false;
        ret=storage_select_node_volume(&reference->node,&previous);
        if(!ret) {
            const struct storage_filesystem_ops *ops=g_storage.fs_ops;
            ret=ops && ops->put_inode ? ops->put_inode(&g_storage,&reference->node,reference->unlinked) : -EOPNOTSUPP;
        }
        storage_io_async_context=saved_async;
    }
    storage_restore_volume(previous);
    if (!ret) {
        struct storage_inode_ref **link = &storage_inode_refs;
        while (*link && *link != reference) link = &(*link)->next;
        if (*link) *link = reference->next;
        kernel_free(reference);
    } else {
        /* Keep failed deletion metadata reserved; do not recycle this inode or
         * unmount the volume as though cleanup had succeeded. */
        console_error("[storage] inode release failed volume=%u inode=%u ret=%d\n",
                       reference->node.volume_id, reference->node.first_cluster, ret);
    }
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/** @brief Refresh the retained inode's current size without reconstructing a name.
 * @param node Borrowed actual inode, updated only after successful provider query.
 * @return Zero or stale/native metadata errno. Caller pins the instance/code;
 * clustered generation cache is execution-owned, other providers enter their
 * serialized stat callback; no graph/module lock spans native I/O.
 */
int storage_inode_refresh(struct storage_node *node)
{
    if (!node) return -22;
    if(storage_inode_clustered(node)) {
        struct storage_inode_ref *ref=storage_inode_for_node(node);
        if(ref){*node=ref->node;return 0;}
        return node->inode_generation ? -116 : 0;
    }
    if (!(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) return 0;
    struct linux_stat_abi info;
    int ret = storage_inode_stat(node, &info);
    if (!ret) node->size = info.st_size;
    return ret;
}

/** @brief Admit and dispatch a write on an already retained native inode.
 * @param node Borrowed mutable actual inode. @param offset Byte position.
 * @param buffer Validated source bytes. @param length Source byte count.
 * @param written Optional output initially zero, provider receives the same pointer.
 * @return Zero or unsupported/lease/provider errno. Recursively enters execution,
 * latches non-replayable mutation before callback, restores native selection;
 * caller retains code/inode, no graph/module lock spans native I/O.
 */
int storage_write_held_node(struct storage_node *node, uint64_t offset,
                            const void *buffer, uint32_t length, uint32_t *written)
{
    if (written) *written = 0;
    if (!node || !(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS |
                                 STORAGE_NODE_FLAG_FAT32 | STORAGE_NODE_FLAG_EXFAT))) return -95;
    uint64_t flags;
    struct storage_volume *previous = NULL;
    kernel_execution_lock_irqsave(&flags);
    int ret = storage_select_node_volume(node, &previous);
    if (!ret) {
        storage_begin_mutation();
        ret = storage_inode_mutation_allowed(node);
        if(!ret) {
            const struct storage_filesystem_ops *ops=g_storage.fs_ops;
            ret=ops && ops->write ? ops->write(&g_storage,node,offset,buffer,length,written) : -EOPNOTSUPP;
        }
    }
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/** @brief Admit and dispatch a resize on an already retained native inode.
 * @param node Borrowed mutable actual inode. @param length Desired byte count.
 * @return Zero or unsupported/lease/provider errno. Recursively enters execution,
 * latches mutation before provider I/O, updates size only after success and
 * restores native selection; caller pins code/inode, no callback lock.
 */
int storage_truncate_held_node(struct storage_node *node, uint64_t length)
{
    if (!node || !(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS |
                                 STORAGE_NODE_FLAG_FAT32 | STORAGE_NODE_FLAG_EXFAT))) return -95;
    uint64_t flags;
    struct storage_volume *previous = NULL;
    kernel_execution_lock_irqsave(&flags);
    int ret = storage_select_node_volume(node, &previous);
    if (!ret) {
        storage_begin_mutation();
        ret=storage_inode_mutation_allowed(node);
        if(!ret) {
            const struct storage_filesystem_ops *ops=g_storage.fs_ops;
            ret=ops && ops->truncate ? ops->truncate(&g_storage,node,length) : -EOPNOTSUPP;
        }
    }
    if (!ret) node->size = length;
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(storage_inode_cache_node);
EXPORT_SYMBOL(storage_inode_find);
EXPORT_SYMBOL(storage_inode_note_data);
EXPORT_SYMBOL(storage_inode_note_move);
EXPORT_SYMBOL(storage_inode_purge_volume);
EXPORT_SYMBOL(storage_inode_unlinked);

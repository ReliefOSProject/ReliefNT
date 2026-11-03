/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <reliefnt/lookup_cache.h>

#define LOOKUP_CACHE_BUCKETS 1024u
#define LOOKUP_CACHE_BUDGET (512u*1024u)

/* Copied identities carry no backing ownership. The generation contract makes
 * them usable only until the next provider mutation; retain still happens in
 * vfs_bind_child. In particular eviction cannot postpone unlink or perform I/O.
 * The intrusive mount list is weak and is purged before the mount is freed. */
struct vfs_lookup_cache_entry {
    struct vfs_lookup_cache_entry *hash_next,**hash_link,*mount_next,**mount_link;
    struct vfs_lookup_cache_entry *older,*newer;
    struct vfs_mount *mount;
    struct storage_node parent,node;
    uint64_t generation;
    uint32_t bytes;
    bool negative;
    char name[];
};
/* Each name is one small heap object backed by at most one slab page. Reclaim
 * can therefore bound physical releases by limiting the number of names. */
_Static_assert(sizeof(struct vfs_lookup_cache_entry)+VFS_NAME_MAX+1<=2048,
               "lookup entries must fit one small heap allocation");
static struct vfs_lookup_cache_entry *buckets[LOOKUP_CACHE_BUCKETS],*oldest,*newest;
static struct vfs_lookup_cache_stats stats={.budget=LOOKUP_CACHE_BUDGET};

/** @brief Hash stable parent fields and an already validated component.
 * @param parent Borrowed live parent. @param name Validated literal name.
 * @return Bucket index. Full provider identity comparison resolves collisions;
 * size and other mutable metadata are never used in the key. No ownership/I/O.
 */
static unsigned cache_bucket(const struct vfs_path *parent,const char *name)
{
    const struct storage_node *node=&parent->dentry->node;
    uint64_t hash=(uintptr_t)parent->mount/16u;
    hash=(hash^node->first_cluster)*1099511628211ULL;
    hash=(hash^node->volume_id)*1099511628211ULL;
    hash=(hash^node->mount_generation)*1099511628211ULL;
    hash=(hash^node->inode_generation)*1099511628211ULL;
    for(;*name;++name)hash=(hash^(uint8_t)*name)*1099511628211ULL;
    return (unsigned)(hash^(hash>>32))&(LOOKUP_CACHE_BUCKETS-1u);
}

/** @brief Unlink a live entry from the global recency queue without freeing it. */
static void cache_unqueue(struct vfs_lookup_cache_entry *entry)
{
    if(entry->older)entry->older->newer=entry->newer;else oldest=entry->newer;
    if(entry->newer)entry->newer->older=entry->older;else newest=entry->older;
}
/** @brief Mark a live unqueued entry most recent; execution writer required. */
static void cache_queue(struct vfs_lookup_cache_entry *entry)
{
    entry->older=newest;entry->newer=NULL;
    if(newest)newest->newer=entry;else oldest=entry;
    newest=entry;
}
/** @brief Remove all weak links before freeing one cache allocation.
 * @param entry Owned linked entry. No provider callback or reference release.
 */
static void cache_drop(struct vfs_lookup_cache_entry *entry)
{
    *entry->hash_link=entry->hash_next;
    if(entry->hash_next)entry->hash_next->hash_link=entry->hash_link;
    *entry->mount_link=entry->mount_next;
    if(entry->mount_next)entry->mount_next->mount_link=entry->mount_link;
    cache_unqueue(entry);--stats.entries;stats.bytes-=entry->bytes;kernel_free(entry);
}

int vfs_lookup_cache_get(const struct vfs_path *parent,const char *name,uint64_t generation,
                          struct storage_node *out)
{
    if(!generation)return 0;
    for(struct vfs_lookup_cache_entry *entry=buckets[cache_bucket(parent,name)],*next;entry;entry=next) {
        next=entry->hash_next;
        if(entry->mount!=parent->mount || __builtin_strcmp(entry->name,name) ||
           !entry->mount->ops->same_node(entry->mount->context,&entry->parent,&parent->dentry->node))continue;
        if(entry->generation!=generation) {
            ++stats.invalidations;cache_drop(entry);break;
        }
        cache_unqueue(entry);cache_queue(entry);
        if(entry->negative){++stats.negative_hits;return -ENOENT;}
        ++stats.hits;*out=entry->node;return 1;
    }
    ++stats.misses;return 0;
}

void vfs_lookup_cache_store(const struct vfs_path *parent,const char *name,uint64_t generation,
                             const struct storage_node *node)
{
    if(!generation)return;
    size_t bytes=sizeof(struct vfs_lookup_cache_entry)+__builtin_strlen(name)+1;
    if(bytes>LOOKUP_CACHE_BUDGET)return;
    while(oldest && stats.bytes>LOOKUP_CACHE_BUDGET-bytes) {
        ++stats.evictions;cache_drop(oldest);
    }
    struct vfs_lookup_cache_entry *entry=kernel_malloc(bytes);
    if(!entry)return; /* Cache pressure never changes a completed lookup. */
    *entry=(struct vfs_lookup_cache_entry){.mount=parent->mount,.parent=parent->dentry->node,
        .generation=generation,.bytes=(uint32_t)bytes,.negative=node==NULL};
    if(node)entry->node=*node;
    __builtin_memcpy(entry->name,name,__builtin_strlen(name)+1);
    unsigned bucket=cache_bucket(parent,name);
    entry->hash_next=buckets[bucket];entry->hash_link=&buckets[bucket];
    if(entry->hash_next)entry->hash_next->hash_link=&entry->hash_next;
    buckets[bucket]=entry;
    entry->mount_next=parent->mount->lookup_cache;entry->mount_link=&parent->mount->lookup_cache;
    if(entry->mount_next)entry->mount_next->mount_link=&entry->mount_next;
    parent->mount->lookup_cache=entry;
    cache_queue(entry);++stats.entries;stats.bytes+=bytes;
}

/** @brief Remove a mount's weak entries before its backend can retire.
 * @param mount Final owned mount under execution ownership. No callbacks/I/O.
 */
void vfs_lookup_cache_purge(struct vfs_mount *mount)
{while(mount->lookup_cache)cache_drop(mount->lookup_cache);}

uint32_t vfs_lookup_cache_reclaim(uint32_t budget)
{
    uint32_t freed=0;
    while(oldest && freed<budget){++stats.evictions;cache_drop(oldest);++freed;}
    return freed;
}
void vfs_lookup_cache_stats(struct vfs_lookup_cache_stats *out)
{*out=stats;}

/* SPDX-License-Identifier: Apache-2.0 */
/* FAT/exFAT held-object I/O. Directory addresses are metadata locations, never
 * ownership: the stable inode generation survives rename and unlink. */
#include <reliefnt/export.h>
#include "../../block/core/storage_internal.h"
#include "../cluster.h"
#include <linux/errno.h>

/** @brief Update a byte range through this instance's cluster transfer primitive.
 * @param ops Borrowed pinned native operations. @param offset Byte position.
 * @param data Optional source bytes, NULL zero-fills. @param length Byte count.
 * @return Zero or native transfer errno. Selected volume and execution-owned
 * chain/data workspace required; no graph/module lock across provider I/O.
 */
static int cluster_range(const struct storage_cluster_ops *ops,uint64_t offset,const uint8_t *data,uint64_t length)
{
    while(length) {
        uint32_t index=(uint32_t)(offset/g_storage.cluster_bytes),at=(uint32_t)(offset%g_storage.cluster_bytes);
        uint32_t take=(uint32_t)min_u64(length,g_storage.cluster_bytes-at);
        int rc=0;
        if(at || take!=g_storage.cluster_bytes)
            rc=ops->io(&g_storage,storage_old_chain[index],storage_cluster_buf,false);
        if(rc)return rc;
        if(data)storage_memcpy(storage_cluster_buf+at,data,take);
        else storage_memzero(storage_cluster_buf+at,take);
        rc=ops->io(&g_storage,storage_old_chain[index],storage_cluster_buf,true);if(rc)return rc;
        offset+=take;length-=take;if(data)data+=take;
    }
    return 0;
}

/** @brief Publish one native data update before retiring old tail allocations.
 * @param ops Borrowed pinned native operations. @param node Actual retained inode.
 * @param next New data/size metadata, borrowed for this call.
 * @return Zero or native metadata errno. Selected volume/execution ownership
 * required. Removed objects have no directory slot; they update only the core
 * generation cache, whose final reference owns backing reclamation.
 */
static int cluster_commit(const struct storage_cluster_ops *ops,struct storage_node *node,const struct storage_node *next)
{
    int rc=storage_inode_is_unlinked(node) ? 0 : ops->publish(&g_storage,node,next);
    if(!rc){storage_inode_note_data(node,next);*node=*next;storage_cache_invalidate();}
    return rc;
}

/** @brief Resize/write the selected FAT/exFAT inode without pathname replay.
 * @param node Owned-by-caller metadata for a held inode; refreshed on commit.
 * @param length New logical length, including writes that preserve old size.
 * @param offset Data offset; ignored if data is NULL.
 * @param data Borrowed write bytes, or NULL for truncate/zero extension.
 * @param bytes Length of data, zero when truncating.
 * @return Zero or negative errno. Execution/mutation transaction and selected
 * volume required. Detached allocations roll back before returning an error.
 */
static int cluster_change(struct storage_node *node,uint64_t length,uint64_t offset,
                            const void *data,uint32_t bytes)
{
    const struct storage_cluster_ops *ops=g_storage.fs_ops ? g_storage.fs_ops->cluster : NULL;
    if(!ops || !ops->collect || !ops->link || !ops->free_one || !ops->io || !ops->publish || !ops->allocate_one)
        return -EOPNOTSUPP;
    if(!g_storage.cluster_bytes || node->type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    if(length>UINT32_MAX)return -EFBIG;
    uint64_t needed=(length+g_storage.cluster_bytes-1)/g_storage.cluster_bytes;
    if(needed>FAT32_MAX_FILE_CLUSTERS)return -EFBIG;
    bool old_nofat=node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT;
    uint32_t old_count=0;int rc=0;
    const struct storage_write_chain_cache *hint=&storage_write_chain_cache;
    uint32_t logical_count=(uint32_t)((node->size+g_storage.cluster_bytes-1)/g_storage.cluster_bytes);
    bool appending=data ? offset>=node->size : length>node->size;
    bool cached=appending && !old_nofat && hint->valid && hint->volume==g_active_volume &&
        hint->inode_generation==node->inode_generation && hint->first_cluster==node->first_cluster &&
        hint->size==node->size && hint->count==logical_count;
    if(cached) {
        old_count=hint->count;
        if(old_count)storage_old_chain[old_count-1]=hint->tail;
    } else rc=ops->collect(&g_storage,node,&old_count);
    /* A failure must never publish a stale tail as reusable append state. */
    storage_write_chain_cache.valid=0;if(rc)return rc;
    uint32_t count=(uint32_t)needed,allocated=old_count;
    if(node->size && (uint64_t)old_count*g_storage.cluster_bytes<node->size)return -EIO;
    bool linked=false;
    for(uint32_t i=old_count;i<count;++i) {
        uint32_t cluster=0;
        rc=ops->allocate_one(&g_storage,&cluster);
        if(rc)goto rollback;
        storage_old_chain[i]=cluster;allocated=i+1;
        storage_memzero(storage_cluster_buf,g_storage.cluster_bytes);
        rc=ops->io(&g_storage,cluster,storage_cluster_buf,true);if(rc)goto rollback;
    }
    if(count>old_count) {
        /* New exFAT clusters may be fragmented. Materialize a real FAT chain
         * before clearing NoFatChain in the directory stream extension. */
        uint32_t begin=old_nofat ? 0 : old_count ? old_count-1 : 0;
        for(uint32_t i=begin;i<count;++i) {
            if(i+1<count && i<old_count)linked=true;
            rc=ops->link(&g_storage,storage_old_chain[i],i+1<count ? storage_old_chain[i+1] :
                ops->eoc);
            if(rc)goto rollback;
        }
    }
    if(length>node->size) {
        uint64_t zero_end=data ? offset : length;
        if(zero_end>node->size) {
            rc=cluster_range(ops,node->size,NULL,zero_end-node->size);if(rc)goto rollback;
        }
    }
    if(data && bytes){rc=cluster_range(ops,offset,data,bytes);if(rc)goto rollback;}
    struct storage_node next=*node;next.size=length;
    next.first_cluster=count ? (old_count ? node->first_cluster : storage_old_chain[0]) : 0;
    if(count>old_count || !count)next.flags &= ~STORAGE_NODE_FLAG_EXFAT_NOFAT;
    rc=cluster_commit(ops,node,&next);if(rc)goto rollback;
    /* The logical size is committed before any old tail is made reusable. */
    if(count<old_count) {
        if(count && !old_nofat) {
            rc=ops->link(&g_storage,storage_old_chain[count-1],ops->eoc);if(rc)return rc;
        }
        for(uint32_t i=count;i<old_count;++i) {
            rc=ops->free_one(&g_storage,storage_old_chain[i]);if(rc)return rc;
        }
    }
    storage_write_chain_cache=(struct storage_write_chain_cache){.volume=g_active_volume,
        .first_cluster=node->first_cluster,.inode_generation=node->inode_generation,
        .size=(uint32_t)node->size,.count=count,.tail=count ? storage_old_chain[count-1] : 0,.valid=1};
    return 0;
rollback:
    if(linked && old_count && !old_nofat) {
        int restore=ops->link(&g_storage,storage_old_chain[old_count-1],ops->eoc);
        /* Do not free clusters still reachable by a failed chain restore. */
        if(restore)return restore;
    }
    for(uint32_t i=old_count;i<allocated;++i) {
        int cleanup=ops->free_one(&g_storage,storage_old_chain[i]);if(cleanup)return cleanup;
    }
    return rc;
}

/** @brief Write a retained cluster inode using its instance's native primitives.
 * @param node Borrowed mutable held inode. @param offset Byte position.
 * @param data Optional validated source, required for nonzero length.
 * @param length Source byte count. @param written Optional actual-count output.
 * @return Zero or validation/overflow/native errno. Selected instance, execution
 * and admitted mutation required; rollback and committed inode identity survive
 * provider errors without replaying names or freeing reachable clusters.
 */
int storage_cluster_write(struct storage_node *node,uint64_t offset,const void *data,
                             uint32_t length,uint32_t *written)
{
    if(written)*written=0;
    if(!node || (!data && length))return -EINVAL;
    int rc=storage_inode_refresh(node);if(rc || !length)return rc;
    uint64_t end=offset+length;if(end<offset)return -EFBIG;
    rc=cluster_change(node,end>node->size ? end : node->size,offset,data,length);
    if(!rc && written)*written=length;return rc;
}

/** @brief Resize a held cluster inode using its instance's native primitives.
 * @param node Borrowed mutable held inode. @param length Desired byte count.
 * @return Zero or validation/native errno. Selected instance, execution and
 * admitted mutation required; old backing retires only after size publication.
 */
int storage_cluster_truncate(struct storage_node *node,uint64_t length)
{
    if(!node)return -EINVAL;int rc=storage_inode_refresh(node);if(rc)return rc;
    return length==node->size ? 0 : cluster_change(node,length,0,NULL,0);
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(storage_cluster_truncate);
EXPORT_SYMBOL(storage_cluster_write);

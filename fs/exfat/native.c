/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../cluster.h"
#include "internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Stamp the actual exFAT root for a retained VFS attachment.
 * @param volume Borrowed parsed instance. @param out Required metadata output.
 * @return Zero or inode-cache errno. Execution ownership and provider pin are
 * required; caller retains any resulting inode separately, no path routing.
 */
static int exfat_native_root(struct storage_volume *volume,struct storage_node *out)
{
    *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,
        .flags=STORAGE_NODE_FLAG_ROOT|STORAGE_NODE_FLAG_EXFAT,
        .volume_id=volume->volume_id,.first_cluster=volume->exfat_root_cluster};
    return storage_inode_cache_node(out);
}
/** @brief Decode one literal child in a selected exFAT stream directory.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param out Required metadata output.
 * @return Zero or codec errno. Execution ownership required across native I/O.
 */
static int exfat_native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                                 const char *name,struct storage_node *out)
{
    (void)volume;return exfat_find_in_dir_ref(parent->first_cluster,
        !!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),name,out,NULL);
}
/** @brief Parse one exclusive selected exfat instance under its constructor pin.
 * @param volume Borrowed unpublished instance with transport geometry.
 * @return Zero or native parser errno. Execution ownership required across I/O;
 * no registry/module lock, no publication or independent ownership transfer.
 */
static int exfat_native_mount(struct storage_volume *volume)
{ (void)volume;return exfat_mount(); }
/** @brief Read the selected provider's actual retained file extent/stream.
 * @param volume Borrowed selected/pinned instance. @param node Retained file.
 * @param offset Byte position. @param bytes Validated writable destination.
 * @param capacity Available bytes, bounded by refreshed inode size.
 * @param length Optional actual-count output, initially zero.
 * @param cursor Optional caller-owned hint. @param mount_flags Access-local flags.
 * @return Zero or negative provider errno. Execution ownership required across
 * native I/O; no graph/module lock, code/instance lifetime held by caller.
 */
static int exfat_native_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                                  void *bytes,uint32_t capacity,uint32_t *length,
                                  struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;if(cursor)cursor->valid=0;
    return exfat_read_node(node,offset,bytes,capacity,length);
}
/** @brief Enumerate the selected provider's actual retained directory.
 * @param volume Borrowed selected/pinned instance. @param node Actual directory.
 * @param cursor Required opaque position. @param entry Required metadata output.
 * @param mount_flags Current attachment/shared flags for this access only.
 * @return One entry, zero EOF or negative native errno. Execution ownership
 * required; no graph/module lock spans native I/O, no retained output pointer.
 */
static int exfat_native_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                                     struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;
    int rc=exfat_iter_dir_entry(node->first_cluster,!!(node->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),*cursor,entry);
    if(!rc){++*cursor;return 1;}return rc==-ENOENT ? 0 : rc;
}
/** @brief Report capacity/identity from the selected native filesystem.
 * @param volume Borrowed selected/pinned instance. @param value Required output
 * initialized with generic flags/name limit/device identity by the caller.
 * @return Zero or native metadata/I/O errno. Execution ownership required;
 * no graph/module lock spans bitmap/group reads, output unspecified on error.
 */
static int exfat_native_statfs(struct storage_volume *volume,struct linux_statfs_abi *value)
{
    int ret=0;
    value->f_type = 0x2011bab0;
    value->f_bsize = volume->cluster_bytes;
    value->f_blocks = volume->exfat_cluster_count;
    for (uint32_t i = 0; i < volume->exfat_cluster_count; ++i) {
        uint8_t allocated;
        ret = exfat_bitmap_get_cached(i + 2, &allocated);
        if (ret < 0) return ret;
        if (!allocated) ++value->f_bfree;
    }
    value->f_bavail = value->f_bfree;
    return 0;
}
/** @brief Retire native inode backing after the final core reference.
 * @param volume Borrowed selected/pinned instance. @param node Actual inode.
 * @param unlinked True if its directory identity was removed while held.
 * @return Zero or native cleanup errno retaining the core inode on failure.
 * Execution ownership required, synchronous non-replayable final cleanup;
 * no module/graph lock spans native I/O and no freed slot is used as identity.
 */
static int exfat_native_put_inode(struct storage_volume *volume,const struct storage_node *node,bool unlinked)
{
    (void)volume;return unlinked ? exfat_free_clusters(node->first_cluster,
        !!(node->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),node->size) : 0;
}
/** @brief Write an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param offset Byte position. @param bytes Validated source bytes.
 * @param length Source byte count. @param written Optional actual-count output.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int exfat_native_write(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
                                   const void *bytes,uint32_t length,uint32_t *written)
{
    (void)volume;return storage_cluster_write(node,offset,bytes,length,written);
}
/** @brief Resize an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param length Desired byte length.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int exfat_native_truncate(struct storage_volume *volume,struct storage_node *node,uint64_t length)
{
    (void)volume;return storage_cluster_truncate(node,length);
}
/** @brief Store one native chain link without changing inode publication order.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Actual native cluster. @param next Next cluster or native EOC.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_link(struct storage_volume *volume,uint32_t cluster,uint32_t next)
{
    (void)volume;return exfat_write_fat(cluster,next);
}
/** @brief Release one detached native allocation after common commit/rollback.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster One detached native cluster to release.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_free_one(struct storage_volume *volume,uint32_t cluster)
{
    return exfat_free_clusters(cluster,1,volume->cluster_bytes);
}
/** @brief Collect the actual native chain into execution-owned common workspace.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual held file. @param count Required chain-count output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_collect(struct storage_volume *volume,const struct storage_node *node,uint32_t *count)
{
    uint64_t needed=(node->size+volume->cluster_bytes-1)/volume->cluster_bytes;
    if(needed>FAT32_MAX_FILE_CLUSTERS)return -EFBIG;
    uint32_t cluster=node->first_cluster;
    for(uint32_t i=0;i<needed;++i) {
        if(!exfat_cluster_valid(cluster))return -EIO;
        for(uint32_t prior=0;prior<i;++prior)if(storage_old_chain[prior]==cluster)return -EIO;
        storage_old_chain[i]=cluster;
        if(i+1<needed) {
            if(node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT)++cluster;
            else {int rc=exfat_read_fat(cluster,&cluster);if(rc)return rc;}
        }
    }
    *count=(uint32_t)needed;return 0;
}
/** @brief Transfer one native cluster using the caller-owned common data buffer.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Native cluster. @param bytes Cluster-sized caller buffer.
 * @param write Store caller bytes if true, otherwise load them.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_io(struct storage_volume *volume,uint32_t cluster,void *bytes,bool write)
{
    return write ? exfat_write_stream(cluster,1,0,bytes,volume->cluster_bytes) :
        exfat_read_stream(cluster,1,0,bytes,volume->cluster_bytes);
}
/** @brief Publish data/size to the actual inode's current native directory record.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Current generation-stamped linked inode. @param next Committed data/size metadata.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_publish(struct storage_volume *volume,const struct storage_node *node,const struct storage_node *next)
{
    (void)volume;int rc=0;
    uint32_t directory=(uint32_t)(node->entry_id>>32)&0x7fffffffu;
    uint32_t position=(uint32_t)node->entry_id;
    uint8_t raw[EXFAT_ENTRY_SIZE];
    struct exfat_dir_ref ref={.directory_cluster=directory,.first_entry=position,
        .directory_nofat=!!(node->entry_id & (UINT64_C(1)<<63))};
    rc=exfat_dir_read_entry(directory,ref.directory_nofat,position,raw);
    if(!rc && (raw[0]!=EXFAT_ENTRY_FILE || raw[1]<2 || raw[1]>19))rc=-ESTALE;
    if(!rc) {
        ref.secondary_count=raw[1];
        rc=exfat_update_entry_data(&ref,next->first_cluster,next->size,
            !!(next->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT));
    }

    return rc;
}
/** @brief Reserve one detached native cluster for common rollback/commit.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Required detached allocation output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_allocate_one(struct storage_volume *volume,uint32_t *cluster)
{
    (void)volume;uint8_t nofat;return exfat_allocate_clusters(1,cluster,&nofat);
}
/** @brief Read native timestamps from the actual retained directory record.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual held inode. @param out Required native timestamp output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_times(struct storage_volume *volume,const struct storage_node *node,struct storage_inode_times *out)
{
    (void)volume;return exfat_inode_times(node,out);
}
/** @brief Store linked-inode access time at this native format's precision.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual linked inode. @param seconds Current UTC seconds.
 * @param actual Required native-precision access-time output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int exfat_native_cluster_access(struct storage_volume *volume,const struct storage_node *node,int64_t seconds,int64_t *actual)
{
    (void)volume;return exfat_inode_access(node,seconds,actual);
}
/** @brief Create an internal or ordinary zero-length native cluster file.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Validated nonexisting component. @param out Required metadata output.
 * @return Zero or native errno. Execution/mutation admission required; if stable
 * inode-key allocation fails after insertion, removes only the newly made record.
 */
static int exfat_native_new_file(struct storage_volume *volume,const struct storage_node *parent,
                                      const char *name,struct storage_node *out)
{
    struct exfat_dir_ref created={0},self;
    int rc=exfat_node_ref(parent,&self);
    if(!rc)rc=exfat_create_entry(parent->first_cluster,!!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),
        parent->flags&STORAGE_NODE_FLAG_ROOT ? NULL : &self,name,0,0,0,0,&created);
    if(!rc) {
        rc=exfat_native_lookup(volume,parent,name,out);
        if(rc){int cleanup=exfat_delete_entry(&created);if(cleanup)rc=cleanup;}
    }
    return rc;
}
/** @brief Roll back one newly made unpublished native cluster member.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Newly created component. @param node Its generation-stamped metadata.
 * @return Zero or native errno. Execution/mutation admission required; disconnects
 * the stable cache key before final backing reuse, never removes an old member.
 */
static int exfat_native_drop_new(struct storage_volume *volume,const struct storage_node *parent,
                                      const char *name,const struct storage_node *node)
{
    (void)volume;struct storage_node found;struct exfat_dir_ref ref;
    int rc=exfat_find_in_dir_ref(parent->first_cluster,!!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),name,&found,&ref);
    if(!rc)rc=exfat_delete_entry(&ref);
    if(!rc && !storage_inode_unlinked(node) && node->first_cluster>=2)
        rc=exfat_free_clusters(node->first_cluster,!!(node->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),node->size);
    return rc;
}
static const struct storage_cluster_ops exfat_native_clusters={
    .new_file=exfat_native_new_file,
    .drop_new=exfat_native_drop_new,
    .eoc=EXFAT_EOC,
    .link=exfat_native_cluster_link,
    .free_one=exfat_native_cluster_free_one,
    .collect=exfat_native_cluster_collect,
    .io=exfat_native_cluster_io,
    .publish=exfat_native_cluster_publish,
    .allocate_one=exfat_native_cluster_allocate_one,
    .times=exfat_native_cluster_times,
    .access=exfat_native_cluster_access,
};
/** @brief Create one authorized native inode beneath a retained parent.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Validated literal component. @param mode Authorized type/mode.
 * @param value Borrowed authorized ownership/mode. @param target Optional symlink target.
 * @param out Required created metadata output.
 * @return Zero or native validation/allocation/I/O errno. Execution/mutation
 * admission required; no callback lock. Core stores sidecar attributes and
 * acquires publication pins before the new VFS dentry becomes observable.
 */
static int exfat_native_create(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                                    const struct reliefos_permissions *value,const char *target,struct storage_node *out)
{
    (void)value;(void)target;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if((mode&LINUX_S_IFMT)==LINUX_S_IFREG) {
        int rc=exfat_native_lookup(volume,parent,name,out);
        if(!rc)return -EEXIST;if(rc!=-ENOENT)return rc;
        return exfat_native_new_file(volume,parent,name,out);
    }
    return (mode&LINUX_S_IFMT)==LINUX_S_IFDIR ? exfat_mkdir_child(parent,name,out) : -EOPNOTSUPP;
}
/** @brief Remove the exact verified native member, retaining open inode backing.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param node Verified victim metadata.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or native errno. Execution/mutation admission required; native
 * orphan/cache retirement precedes any final reuse, no callback lock across I/O.
 */
static int exfat_native_remove(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                                    const struct storage_node *node,bool directory)
{
    (void)volume;(void)node;return exfat_remove_child(parent,name,directory);
}
/** @brief Rename verified native members using the core's preheld graph operands.
 * @param volume Borrowed selected/pinned instance. @param args Required borrowed
 * operands and commit output, valid only during this synchronous callback.
 * @return Zero or native errno. Execution/mutation admission required; reports
 * irreversible commit even if later persistence fails, no callback lock.
 */
static int exfat_native_rename(struct storage_volume *volume,const struct storage_rename_args *args)
{
    (void)volume;return exfat_rename_child(args->old_parent,args->old_name,args->new_parent,args->new_name,args->committed);
}
/** @brief Retire native global cache hints before an instance drops its code pin.
 * @param volume Exclusive quiesced/unmounted instance, borrowed.
 * @return None. Execution ownership required; cache invalidation allocates and
 * waits for nothing. The module's final resource cleanup owns backing release.
 */
static void exfat_native_destroy(struct storage_volume *volume)
{ (void)volume;exfat_cache_invalidate(); }
const struct storage_filesystem_ops storage_exfat_ops={
    .destroy=exfat_native_destroy,
    .invalidate=exfat_cache_invalidate,
    .create=exfat_native_create,
    .remove=exfat_native_remove,
    .rename=exfat_native_rename,
    .flags=STORAGE_FS_SIDECAR,
    .put_inode=exfat_native_put_inode,
    .write=exfat_native_write,
    .truncate=exfat_native_truncate,
    .cluster=&exfat_native_clusters,
    .statfs=exfat_native_statfs,
    .name="exfat",
    .owner=THIS_MODULE,
    .mount=exfat_native_mount,
    .read=exfat_native_read,
    .readdir=exfat_native_readdir,
    .root=exfat_native_root,
    .lookup=exfat_native_lookup
};

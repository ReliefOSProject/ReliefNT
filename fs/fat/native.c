/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../cluster.h"
#include "internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Stamp the actual FAT root for a retained VFS attachment.
 * @param volume Borrowed parsed FAT volume. @param out Required metadata output.
 * @return Zero or inode-cache allocation/generation errno. Execution ownership
 * required; caller pins provider code before entry; no global path resolution.
 */
static int fat_native_root(struct storage_volume *volume,struct storage_node *out)
{
    *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,
        .flags=STORAGE_NODE_FLAG_ROOT|STORAGE_NODE_FLAG_FAT32,
        .volume_id=volume->volume_id,.first_cluster=volume->root_cluster};
    return storage_inode_cache_node(out);
}
/** @brief Decode one literal child in this selected FAT directory.
 * @param volume Borrowed selected/pinned native instance.
 * @param parent Borrowed actual directory node. @param name Literal component.
 * @param out Required caller-owned metadata output.
 * @return Zero or codec errno; execution ownership required across native I/O.
 */
static int fat_native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                               const char *name,struct storage_node *out)
{ (void)volume;return fat32_find_in_dir(parent->first_cluster,name,out); }

/** @brief Parse one exclusive selected vfat instance under its constructor pin.
 * @param volume Borrowed unpublished instance with transport geometry.
 * @return Zero or native parser errno. Execution ownership required across I/O;
 * no registry/module lock, no publication or independent ownership transfer.
 */
static int fat_native_mount(struct storage_volume *volume)
{ (void)volume;return fat32_mount(); }
/** @brief Read the selected provider's actual retained file extent/stream.
 * @param volume Borrowed selected/pinned instance. @param node Retained file.
 * @param offset Byte position. @param bytes Validated writable destination.
 * @param capacity Available bytes, bounded by refreshed inode size.
 * @param length Optional actual-count output, initially zero.
 * @param cursor Optional caller-owned hint. @param mount_flags Access-local flags.
 * @return Zero or negative provider errno. Execution ownership required across
 * native I/O; no graph/module lock, code/instance lifetime held by caller.
 */
static int fat_native_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                                  void *bytes,uint32_t capacity,uint32_t *length,
                                  struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;
    uint8_t *dst=bytes;uint32_t done=0;int ret;
    uint32_t cluster = node->first_cluster;
    uint64_t skip_clusters = offset / g_storage.cluster_bytes;
    uint32_t cluster_off = (uint32_t)(offset % g_storage.cluster_bytes);
    /* FAT32 directory/file reads alternate between the installer root and a
     * mounted /target or /target/boot volume. A cached cluster cursor can
     * survive a volume switch and turn a valid fragmented file into EIO.
     * FAT cursors are an optimization only; re-walk the chain for each
     * syscall slice until the read cache can be keyed by volume as well. */
    if (cursor) cursor->valid = 0;
    if (cursor && cursor->valid) {
        cluster = cursor->cluster;
    } else {
        while (skip_clusters--) {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {

                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC || next < 2u) {
                console_error("[storage] fat skip invalid cluster=%u next=%u off=%llu\n",
                               cluster, next, (unsigned long long)offset);

                return -5;
            }
            cluster = next;
        }
    }

    while (done < capacity) {
        uint64_t cluster_file_offset = offset + done - cluster_off;
        uint64_t bytes_from_cluster = node->size - cluster_file_offset;
        /* Include the leading offset because cache fills are cluster-aligned. */
        uint64_t bytes_requested = capacity - done + cluster_off;
        uint32_t max_clusters;
        uint32_t cache_offset;
        uint32_t cache_clusters;
        uint32_t next;
        uint32_t take;
        if (bytes_from_cluster > bytes_requested) {
            bytes_from_cluster = bytes_requested;
        }
        max_clusters = (uint32_t)((bytes_from_cluster +
                                   g_storage.cluster_bytes - 1u) /
                                  g_storage.cluster_bytes);
        if (max_clusters > STORAGE_READAHEAD_SECTORS / g_storage.sectors_per_cluster) {
            max_clusters = STORAGE_READAHEAD_SECTORS / g_storage.sectors_per_cluster;
        }
        if (storage_read_cache_lookup(cluster, &cache_offset, &cache_clusters)) {
            if (cache_clusters > max_clusters) {
                cache_clusters = max_clusters;
            }
            ret = fat32_read_fat_entry(cluster + cache_clusters - 1u, &next);
            if (ret < 0) {

                return storage_read_failure(ret);
            }
        } else {
            ret = storage_read_contiguous_clusters(cluster, max_clusters,
                                                   &cache_clusters, &next);
            if (ret < 0) {

                return storage_read_failure(ret);
            }
            ret = storage_read_cache_fill(cluster, cache_clusters);
            if (ret < 0) {

                return storage_read_failure(ret);
            }
            cache_offset = 0;
        }
        take = cache_clusters * g_storage.cluster_bytes - cluster_off;
        if (take > capacity - done) {
            take = capacity - done;
        }
        storage_memcpy(dst + done, storage_read_cache_data + cache_offset + cluster_off,
                       take);
        done += take;
        if (done >= capacity) {
            if (cursor && offset + done < node->size) {
                uint32_t consumed = cluster_off + take;
                uint32_t advanced = consumed / g_storage.cluster_bytes;
                uint32_t next_cluster = cluster + advanced;
                if ((consumed % g_storage.cluster_bytes) == 0u &&
                    advanced >= cache_clusters) {
                    next_cluster = next;
                }
                if (next_cluster < 2u || next_cluster >= FAT32_EOC) {
                    console_error("[storage] fat cursor invalid cluster=%u next=%u next_cluster=%u advanced=%u cache_clusters=%u consumed=%u off=%llu size=%llu\n",
                                   cluster, next, next_cluster, advanced, cache_clusters,
                                   consumed, (unsigned long long)offset,
                                   (unsigned long long)node->size);
                    cursor->valid = 0;

                    return -5;
                }
                cursor->offset = offset + done;
                cursor->cluster = next_cluster;
                cursor->valid = 1;
            } else if (cursor) {
                cursor->valid = 0;
            }
            break;
        }
        if (next >= FAT32_EOC) {
            console_error("[storage] fat mid eoc cluster=%u next=%u done=%u capacity=%u off=%llu size=%llu\n",
                           cluster, next, done, capacity, (unsigned long long)offset,
                           (unsigned long long)node->size);
            if (cursor) {
                cursor->valid = 0;
            }

            return -5;
        }
        if (next < 2u) {
            console_error("[storage] fat mid bad cluster=%u next=%u done=%u capacity=%u off=%llu size=%llu\n",
                           cluster, next, done, capacity, (unsigned long long)offset,
                           (unsigned long long)node->size);
            if (cursor) {
                cursor->valid = 0;
            }

            return -5;
        }
        if (cursor) {
            cursor->offset = offset + done;
            cursor->cluster = next;
            cursor->valid = 1;
        }
        cluster = next;
        cluster_off = 0;
    }
    if (length) {
        *length = done;
    }

    return 0;
}
/** @brief Enumerate the selected provider's actual retained directory.
 * @param volume Borrowed selected/pinned instance. @param node Actual directory.
 * @param cursor Required opaque position. @param entry Required metadata output.
 * @param mount_flags Current attachment/shared flags for this access only.
 * @return One entry, zero EOF or negative native errno. Execution ownership
 * required; no graph/module lock spans native I/O, no retained output pointer.
 */
static int fat_native_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                                     struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;
    int rc=fat32_iter_dir_entry(node->first_cluster,*cursor,entry);
    if(!rc){++*cursor;return 1;}return rc==-ENOENT ? 0 : rc;
}
/** @brief Report capacity/identity from the selected native filesystem.
 * @param volume Borrowed selected/pinned instance. @param value Required output
 * initialized with generic flags/name limit/device identity by the caller.
 * @return Zero or native metadata/I/O errno. Execution ownership required;
 * no graph/module lock spans bitmap/group reads, output unspecified on error.
 */
static int fat_native_statfs(struct storage_volume *volume,struct linux_statfs_abi *value)
{
    int ret=0;
    value->f_type = 0x4d44;
    value->f_bsize = volume->cluster_bytes;
    value->f_blocks = volume->data_cluster_count;
    if (volume->fat_fsinfo_valid) value->f_bfree = volume->fat_free_clusters;
    else for (uint32_t i = 0; i < volume->data_cluster_count; ++i) {
        uint32_t entry;
        ret = fat32_read_fat_entry(i + 2, &entry);
        if (ret < 0) return ret;
        if (!entry) ++value->f_bfree;
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
static int fat_native_put_inode(struct storage_volume *volume,const struct storage_node *node,bool unlinked)
{
    (void)volume;return unlinked && node->first_cluster>=2 ? fat32_free_chain(node->first_cluster) : 0;
}
/** @brief Write an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param offset Byte position. @param bytes Validated source bytes.
 * @param length Source byte count. @param written Optional actual-count output.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int fat_native_write(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
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
static int fat_native_truncate(struct storage_volume *volume,struct storage_node *node,uint64_t length)
{
    (void)volume;return storage_cluster_truncate(node,length);
}
/** @brief Store one native chain link without changing inode publication order.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Actual native cluster. @param next Next cluster or native EOC.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_link(struct storage_volume *volume,uint32_t cluster,uint32_t next)
{
    (void)volume;return fat32_write_fat_entry(cluster,next);
}
/** @brief Release one detached native allocation after common commit/rollback.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster One detached native cluster to release.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_free_one(struct storage_volume *volume,uint32_t cluster)
{
    (void)volume;int rc=fat32_write_fat_entry(cluster,0);
    if(!rc)rc=fat32_note_freed();return rc;
}
/** @brief Collect the actual native chain into execution-owned common workspace.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual held file. @param count Required chain-count output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_collect(struct storage_volume *volume,const struct storage_node *node,uint32_t *count)
{
    (void)volume;return fat32_collect_chain(node->first_cluster,storage_old_chain,count);
}
/** @brief Transfer one native cluster using the caller-owned common data buffer.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Native cluster. @param bytes Cluster-sized caller buffer.
 * @param write Store caller bytes if true, otherwise load them.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_io(struct storage_volume *volume,uint32_t cluster,void *bytes,bool write)
{
    (void)volume;return write ? fat32_write_cluster(cluster,bytes) : fat32_read_cluster(cluster,bytes);
}
/** @brief Publish data/size to the actual inode's current native directory record.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Current generation-stamped linked inode. @param next Committed data/size metadata.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_publish(struct storage_volume *volume,const struct storage_node *node,const struct storage_node *next)
{
    (void)volume;int rc=0;
    uint32_t directory=(uint32_t)(node->entry_id>>32)&0x7fffffffu;
    uint32_t position=(uint32_t)node->entry_id;
    struct fat32_dir_ref ref={.entry_cluster=directory,.entry_offset=position};
    if(!directory || position>volume->cluster_bytes-sizeof(ref.dirent))return -ESTALE;
    rc=fat32_read_cluster(directory,storage_cluster_buf);
    if(!rc) {
        storage_memcpy(&ref.dirent,storage_cluster_buf+position,sizeof(ref.dirent));
        if(!ref.dirent.name[0] || ref.dirent.name[0]==0xe5)rc=-ESTALE;
    }
    if(!rc) {
        ref.dirent.first_cluster_hi=(uint16_t)(next->first_cluster>>16);
        ref.dirent.first_cluster_lo=(uint16_t)next->first_cluster;
        ref.dirent.size=(uint32_t)next->size;rc=fat32_update_dirent(&ref);
    }
    return rc;
}
/** @brief Reserve one detached native cluster for common rollback/commit.
 * @param volume Borrowed selected/pinned native instance.
 * @param cluster Required detached allocation output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_allocate_one(struct storage_volume *volume,uint32_t *cluster)
{
    (void)volume;int rc=fat32_find_free_cluster(cluster);
    if(!rc)rc=fat32_write_fat_entry(*cluster,FAT32_EOC);
    if(!rc)rc=fat32_note_allocated();return rc;
}
/** @brief Read native timestamps from the actual retained directory record.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual held inode. @param out Required native timestamp output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_times(struct storage_volume *volume,const struct storage_node *node,struct storage_inode_times *out)
{
    (void)volume;return fat32_inode_times(node,out);
}
/** @brief Store linked-inode access time at this native format's precision.
 * @param volume Borrowed selected/pinned native instance.
 * @param node Actual linked inode. @param seconds Current UTC seconds.
 * @param actual Required native-precision access-time output.
 * @return Zero or native validation/I/O/allocation errno. Execution ownership
 * required; no graph/module lock spans I/O, no stack/workspace pointer retained.
 */
static int fat_native_cluster_access(struct storage_volume *volume,const struct storage_node *node,int64_t seconds,int64_t *actual)
{
    (void)volume;return fat32_inode_access(node,seconds,actual);
}
/** @brief Create an internal or ordinary zero-length native cluster file.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Validated nonexisting component. @param out Required metadata output.
 * @return Zero or native errno. Execution/mutation admission required; if stable
 * inode-key allocation fails after insertion, removes only the newly made record.
 */
static int fat_native_new_file(struct storage_volume *volume,const struct storage_node *parent,
                                      const char *name,struct storage_node *out)
{
    int rc=fat32_create_dirent(parent->first_cluster,name,FAT32_ATTR_ARCHIVE,0,0);
    if(!rc) {
        rc=fat_native_lookup(volume,parent,name,out);
        if(rc){int cleanup=fat32_delete_dirent(parent->first_cluster,name,NULL);if(cleanup)rc=cleanup;}
    }
    return rc;
}
/** @brief Roll back one newly made unpublished native cluster member.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Newly created component. @param node Its generation-stamped metadata.
 * @return Zero or native errno. Execution/mutation admission required; disconnects
 * the stable cache key before final backing reuse, never removes an old member.
 */
static int fat_native_drop_new(struct storage_volume *volume,const struct storage_node *parent,
                                      const char *name,const struct storage_node *node)
{
    (void)volume;int rc=fat32_delete_dirent(parent->first_cluster,name,NULL);
    if(!rc && !storage_inode_unlinked(node) && node->first_cluster>=2)rc=fat32_free_chain(node->first_cluster);
    return rc;
}
static const struct storage_cluster_ops fat_native_clusters={
    .new_file=fat_native_new_file,
    .drop_new=fat_native_drop_new,
    .eoc=FAT32_EOC,
    .link=fat_native_cluster_link,
    .free_one=fat_native_cluster_free_one,
    .collect=fat_native_cluster_collect,
    .io=fat_native_cluster_io,
    .publish=fat_native_cluster_publish,
    .allocate_one=fat_native_cluster_allocate_one,
    .times=fat_native_cluster_times,
    .access=fat_native_cluster_access,
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
static int fat_native_create(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                                    const struct reliefos_permissions *value,const char *target,struct storage_node *out)
{
    (void)value;(void)target;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if((mode&LINUX_S_IFMT)==LINUX_S_IFREG) {
        int rc=fat_native_lookup(volume,parent,name,out);
        if(!rc)return -EEXIST;if(rc!=-ENOENT)return rc;
        return fat_native_new_file(volume,parent,name,out);
    }
    return (mode&LINUX_S_IFMT)==LINUX_S_IFDIR ? fat32_mkdir_child(parent,name,out) : -EOPNOTSUPP;
}
/** @brief Remove the exact verified native member, retaining open inode backing.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param node Verified victim metadata.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or native errno. Execution/mutation admission required; native
 * orphan/cache retirement precedes any final reuse, no callback lock across I/O.
 */
static int fat_native_remove(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                                    const struct storage_node *node,bool directory)
{
    (void)volume;(void)node;return fat32_remove_child(parent,name,directory);
}
/** @brief Rename verified native members using the core's preheld graph operands.
 * @param volume Borrowed selected/pinned instance. @param args Required borrowed
 * operands and commit output, valid only during this synchronous callback.
 * @return Zero or native errno. Execution/mutation admission required; reports
 * irreversible commit even if later persistence fails, no callback lock.
 */
static int fat_native_rename(struct storage_volume *volume,const struct storage_rename_args *args)
{
    (void)volume;return fat32_rename_child(args->old_parent,args->old_name,args->new_parent,args->new_name,args->committed);
}
const struct storage_filesystem_ops storage_fat_ops={
    .create=fat_native_create,
    .remove=fat_native_remove,
    .rename=fat_native_rename,
    .flags=STORAGE_FS_SIDECAR,
    .put_inode=fat_native_put_inode,
    .write=fat_native_write,
    .truncate=fat_native_truncate,
    .cluster=&fat_native_clusters,
    .statfs=fat_native_statfs,
    .name="vfat",
    .owner=THIS_MODULE,
    .mount=fat_native_mount,
    .read=fat_native_read,
    .readdir=fat_native_readdir,
    .root=fat_native_root,
    .lookup=fat_native_lookup
};

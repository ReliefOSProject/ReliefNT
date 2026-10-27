/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/export.h>
#include <reliefnt/module.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/fs_type.h>

/** @brief Decode the ext-family root with the native feature-checked parser.
 * @param volume Borrowed pinned instance. @param out Required metadata output.
 * @return Zero or native inode/I/O errno. Execution ownership required; no
 * registry/module/graph lock spans I/O and no global pathname is resolved.
 */
static int ext_native_root(struct storage_volume *volume,struct storage_node *out)
{ return storage_ext4_ops.lookup(volume,"/",out); }
/** @brief Decode one literal child from a retained ext-family parent inode.
 * @param volume Borrowed pinned instance. @param parent Actual directory node.
 * @param name Literal component. @param out Required caller metadata output.
 * @return Zero or native validation/lookup/inode errno. Execution ownership
 * required across native I/O; never resolves a diagnostic mount pathname.
 */
static int ext_native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                               const char *name,struct storage_node *out)
{
    struct ext4_inode_view directory,found;uint64_t ino=0;
    int rc=storage_ext4_check_node(volume,parent,&directory);
    if(!rc && (directory.mode&0170000)!=0040000)rc=-ENOTDIR;
    if(!rc)rc=storage_ext4_dir_lookup(volume,parent->first_cluster,name,&ino,NULL);
    if(!rc)rc=storage_ext4_read_inode(volume,ino,&found);
    if(!rc)storage_ext4_node_from_inode(volume,ino,&found,out);
    return rc;
}
/** @brief Read a retained ext-family symlink without name resolution.
 * @param volume Borrowed pinned instance. @param node Retained symlink inode.
 * @param bytes Required output. @param capacity Available bytes.
 * @param length Required actual byte count output.
 * @return Zero or native errno. Execution ownership required across I/O.
 */
static int ext_native_readlink(struct storage_volume *volume,const struct storage_node *node,
                                 char *bytes,uint32_t capacity,uint32_t *length)
{ return storage_ext4_ops.readlink(volume,node,bytes,capacity,length); }
/** @brief Parse one exclusive selected ext4 instance under its constructor pin.
 * @param volume Borrowed unpublished instance with transport geometry.
 * @return Zero or native parser errno. Execution ownership required across I/O;
 * no registry/module lock, no publication or independent ownership transfer.
 */
static int ext_native_mount(struct storage_volume *volume)
{ (void)volume;return storage_ext4_mount(volume); }
/** @brief Close quiesced ext private state and clean identities before owner release.
 * @param volume Exclusive unmounted instance with all inode users retired.
 * @return None. Caller completed sync/purge; no module lock or queued callbacks.
 */
static void ext_native_destroy(struct storage_volume *volume)
{ storage_ext4_state_reset(volume);ext2_cache_reset(volume); }
/** @brief Read the selected provider's actual retained file extent/stream.
 * @param volume Borrowed selected/pinned instance. @param node Retained file.
 * @param offset Byte position. @param bytes Validated writable destination.
 * @param capacity Available bytes, bounded by refreshed inode size.
 * @param length Optional actual-count output, initially zero.
 * @param cursor Optional caller-owned hint. @param mount_flags Access-local flags.
 * @return Zero or negative provider errno. Execution ownership required across
 * native I/O; no graph/module lock, code/instance lifetime held by caller.
 */
static int ext_native_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                                  void *bytes,uint32_t capacity,uint32_t *length,
                                  struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    (void)mount_flags;if(cursor)cursor->valid=0;
    return storage_ext4_ops.read(volume,node,offset,bytes,capacity,length);
}
/** @brief Enumerate the selected provider's actual retained directory.
 * @param volume Borrowed selected/pinned instance. @param node Actual directory.
 * @param cursor Required opaque position. @param entry Required metadata output.
 * @param mount_flags Current attachment/shared flags for this access only.
 * @return One entry, zero EOF or negative native errno. Execution ownership
 * required; no graph/module lock spans native I/O, no retained output pointer.
 */
static int ext_native_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                                     struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)mount_flags;return storage_ext4_ops.readdir(volume,node,cursor,entry);
}
/** @brief Store an authorized ext inode through a bounded journal transaction.
 * @param v Borrowed selected instance. @param ino Native inode number.
 * @param in Borrowed full updated inode image.
 * @return Zero or journal/write errno. Execution ownership and provider pin
 * required across synchronous metadata I/O; journal errors remain observable.
 */
static int ext_native_store_inode(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h);
    if (ret<0) return ret;
    ret=storage_ext4_write_inode(v,ino,in);
    if (ret<0) storage_ext4_journal_abort(&h,ret);
    int stopped=storage_ext4_journal_stop(&h); return ret<0?ret:stopped;
}
/** @brief Query or update authorized native ext ownership/mode metadata.
 * @param volume Borrowed selected instance. @param node Actual retained inode.
 * @param value Required result or authorized new metadata. @param write Store if true.
 * @return Zero or native inode/journal errno. Caller validates permissions;
 * execution ownership required, no graph/module lock spans native metadata I/O.
 */
static int ext_native_permissions(struct storage_volume *volume,const struct storage_node *node,
                                    struct reliefos_permissions *value,bool write)
{
    struct ext4_inode_view inode;int ret;
    ret = storage_ext4_check_node(volume, node, &inode);
    if (ret < 0) return ret;
    if (write) {
        struct reliefos_time_info now;
        if (time_wall_clock(&now) == 0) inode.ctime = (uint32_t)now.unix_seconds;
        inode.mode = (inode.mode & 0170000u) | (value->mode & 07777u);
        inode.uid = value->uid;
        inode.gid = value->gid;
        ret = ext_native_store_inode(volume, node->first_cluster, &inode);
    } else {
        value->mode = inode.mode & 07777u;
        value->uid = inode.uid;
        value->gid = inode.gid;
    }
    return ret;
}
/** @brief Report the retained ext inode's full native Linux stat metadata.
 * @param volume Borrowed pinned instance. @param node Actual retained inode.
 * @param value Required caller-owned stat output, unspecified on error.
 * @return Zero or native inode errno. Execution ownership required across I/O;
 * generation, high uid/gid words and native device-node identity are preserved.
 */
static int ext_native_stat(struct storage_volume *volume,const struct storage_node *node,struct linux_stat_abi *value)
{
    struct ext4_inode_view inode;int rc=storage_ext4_check_node(volume,node,&inode);if(rc)return rc;
    *value = (struct linux_stat_abi){0};
    value->st_dev = storage_volume_device_number(volume);
    value->st_ino = node->first_cluster;
    value->st_nlink = inode.links_count;
    value->st_mode = inode.mode;
    value->st_uid = inode.uid;
    value->st_gid = inode.gid;
    value->st_size = inode.size;
    value->st_blksize = volume->ext4.block_size;
    value->st_blocks = inode.blocks;
    value->atime_sec = (int32_t)inode.atime;
    value->mtime_sec = (int32_t)inode.mtime;
    value->ctime_sec = (int32_t)inode.ctime;
    if ((inode.mode & LINUX_S_IFMT) == LINUX_S_IFCHR || (inode.mode & LINUX_S_IFMT) == LINUX_S_IFBLK) {
        uint32_t dev = ext4_get_le32(inode.i_block_raw);
        if (dev) value->st_rdev = dev;
        else value->st_rdev = ext4_get_le32(inode.i_block_raw+4);
    }
    return 0;
}
/** @brief Store authorized ext access/modification times on the actual inode.
 * @param volume Borrowed pinned instance. @param node Actual retained inode.
 * @param atime New native access seconds. @param mtime New modification seconds.
 * @param set_atime Whether to store access time. @param set_mtime Whether to store modification time.
 * @return Zero or native inode/journal errno. Execution ownership and prior
 * authorization required; no graph/module lock spans journal I/O.
 */
static int ext_native_utimens(struct storage_volume *volume,const struct storage_node *node,int64_t atime,int64_t mtime,
                                bool set_atime,bool set_mtime)
{
    struct ext4_inode_view inode;int rc=storage_ext4_check_node(volume,node,&inode);if(rc)return rc;
    if(set_atime)inode.atime=(uint32_t)atime;
    if(set_mtime)inode.mtime=(uint32_t)mtime;
    return ext_native_store_inode(volume,node->first_cluster,&inode);
}
/** @brief Classify ext2 sparse-super groups without backend or namespace state.
 * @param group Native group number. @param sparse Enable sparse-super layout.
 * @return True for groups carrying a superblock copy. Pure layout arithmetic.
 */
static bool ext_native_super_group(uint32_t group, bool sparse)
{
    if (!sparse || group <= 1) return true;
    const uint32_t bases[] = {3, 5, 7};
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t n = group;
        while (n > 1 && n % bases[i] == 0) n /= bases[i];
        if (n == 1) return true;
    }
    return false;
}

/** @brief Report capacity/identity from the selected native filesystem.
 * @param volume Borrowed selected/pinned instance. @param value Required output
 * initialized with generic flags/name limit/device identity by the caller.
 * @return Zero or native metadata/I/O errno. Execution ownership required;
 * no graph/module lock spans bitmap/group reads, output unspecified on error.
 */
static int ext_native_statfs(struct storage_volume *volume,struct linux_statfs_abi *value)
{
    int ret=0;
    if(volume->filesystem==STORAGE_FILESYSTEM_EXT4)return storage_ext4_ops.statfs(volume,value);
    struct ext2_superblock super;
    ret = storage_read_sectors(volume->ext2_start_lba + 2, 2, storage_scratch);
    if (ret < 0) return ret;
    storage_memcpy(&super, storage_scratch, sizeof(super));
    if (super.magic != EXT2_SUPER_MAGIC) { ret = -5; return ret; }
    value->f_type = EXT2_SUPER_MAGIC;
    value->f_bsize = volume->ext2_block_size;
    uint64_t overhead = super.first_data_block;
    uint32_t tables = (volume->ext2_group_count * sizeof(struct ext2_group_desc) + value->f_bsize - 1) / value->f_bsize;
    uint64_t inode_blocks = ((uint64_t)volume->ext2_inodes_per_group * volume->ext2_inode_size + value->f_bsize - 1) / value->f_bsize;
    for (uint32_t i = 0; i < volume->ext2_group_count; ++i) {
        struct storage_ext4_group_view group;
        ret = storage_ext4_read_group(volume,i,&group);
        if (ret < 0) return ret;
        value->f_bfree += group.free_blocks_count;
        value->f_ffree += group.free_inodes_count;
        if (ext_native_super_group(i, super.feature_ro_compat & 1)) overhead += 1 + tables;
    }
    overhead += volume->ext2_group_count * (2 + inode_blocks);
    if (overhead > super.blocks_count) { ret = -5; return ret; }
    value->f_blocks = super.blocks_count - overhead;
    value->f_bavail = value->f_bfree > super.reserved_blocks_count ? value->f_bfree - super.reserved_blocks_count : 0;
    value->f_files = super.inodes_count;
    value->f_fsid[0] = storage_get_u32(super.uuid) ^ storage_get_u32(super.uuid + 8);
    value->f_fsid[1] = storage_get_u32(super.uuid + 4) ^ storage_get_u32(super.uuid + 12);
    return 0;
}
/** @brief Validate/retain one native inode for a new core inode reference.
 * @param volume Borrowed selected/pinned instance. @param node Actual inode.
 * @return Zero or native inode/allocation errno. Execution ownership required;
 * success pairs with put_inode on the final core reference, no callback lock.
 */
static int ext_native_hold_inode(struct storage_volume *volume,const struct storage_node *node)
{
    struct ext4_inode_view inode;int rc=storage_ext4_check_node(volume,node,&inode);
    return rc ? rc : inode.mode ? 0 : -ENOENT;
}
/** @brief Retire native inode backing after the final core reference.
 * @param volume Borrowed selected/pinned instance. @param node Actual inode.
 * @param unlinked True if its directory identity was removed while held.
 * @return Zero or native cleanup errno retaining the core inode on failure.
 * Execution ownership required, synchronous non-replayable final cleanup;
 * no module/graph lock spans native I/O and no freed slot is used as identity.
 */
static int ext_native_put_inode(struct storage_volume *volume,const struct storage_node *node,bool unlinked)
{
    if(!unlinked)return 0;
    struct ext4_inode_view inode;int rc=storage_ext4_check_node(volume,node,&inode);if(rc)return rc;
    if(inode.links_count)return 0;
    page_cache_invalidate_node(node);
    return storage_ext4_destroy_inode(volume,node->first_cluster);
}
/** @brief Write an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param offset Byte position. @param bytes Validated source bytes.
 * @param length Source byte count. @param written Optional actual-count output.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int ext_native_write(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
                                   const void *bytes,uint32_t length,uint32_t *written)
{
    return storage_ext4_ops.write(volume,node,offset,bytes,length,written);
}
/** @brief Resize an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param length Desired byte length.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int ext_native_truncate(struct storage_volume *volume,struct storage_node *node,uint64_t length)
{
    return storage_ext4_ops.truncate(volume,node,length);
}
/** @brief Create one authorized native inode beneath a retained parent.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Validated literal component. @param mode Authorized type/mode.
 * @param value Borrowed authorized ownership/mode. @param target Optional symlink target.
 * @param out Required created metadata output.
 * @return Zero or native validation/allocation/I/O errno. Execution/mutation
 * admission required; no callback lock. Core stores sidecar attributes and
 * acquires publication pins before the new VFS dentry becomes observable.
 */
static int ext_native_create(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                                    const struct reliefos_permissions *value,const char *target,struct storage_node *out)
{
    struct ext4_inode_view directory;int rc=storage_ext4_check_node(volume,parent,&directory);
    if(!rc && (directory.mode&LINUX_S_IFMT)!=LINUX_S_IFDIR)rc=-ENOTDIR;
    if(!rc && !directory.links_count)rc=-ENOENT;
    if(!rc)rc=storage_ext4_create_node(volume,parent->first_cluster,name,(uint16_t)mode,value->uid,value->gid,target,out);
    return rc;
}
/** @brief Remove the exact verified native member, retaining open inode backing.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param node Verified victim metadata.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or native errno. Execution/mutation admission required; native
 * orphan/cache retirement precedes any final reuse, no callback lock across I/O.
 */
static int ext_native_remove(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                                    const struct storage_node *node,bool directory)
{
    struct storage_inode_ref *pin=storage_inode_find(volume->volume_id,node->first_cluster);
    bool held=pin && pin->references;
    int rc=storage_ext4_unlink_held(volume,parent->first_cluster,name,directory,held);
    if(!rc && held)pin->unlinked=true;
    if(!rc && !held)page_cache_invalidate_node(node);
    return rc;
}
/** @brief Rename verified native members using the core's preheld graph operands.
 * @param volume Borrowed selected/pinned instance. @param args Required borrowed
 * operands and commit output, valid only during this synchronous callback.
 * @return Zero or native errno. Execution/mutation admission required; reports
 * irreversible commit even if later persistence fails, no callback lock.
 */
static int ext_native_rename(struct storage_volume *volume,const struct storage_rename_args *args)
{
    struct storage_inode_ref *pin=args->target ? storage_inode_find(volume->volume_id,args->target->first_cluster) : NULL;
    bool held=pin && pin->references;
    int rc=storage_ext4_rename_held(volume,args->old_parent->first_cluster,args->old_name,
        args->new_parent->first_cluster,args->new_name,held);
    *args->committed=!rc;
    if(!rc && held)pin->unlinked=true;
    if(!rc && args->target && !held)page_cache_invalidate_node(args->target);
    return rc;
}
/** @brief Publish a hard link to the actual retained native inode.
 * @param volume Borrowed selected/pinned instance. @param source Actual inode.
 * @param parent Retained destination directory. @param name Validated component.
 * @return Zero or native errno. Caller checked permissions/link eligibility;
 * execution/mutation admission required, no graph/module lock across native I/O.
 */
static int ext_native_link(struct storage_volume *volume,const struct storage_node *source,
                                  const struct storage_node *parent,const char *name)
{
    return storage_ext4_link(volume,parent->first_cluster,name,source->first_cluster);
}
/** @brief Evict native raw aliases before a physical sector write is submitted.
 * @param writer Borrowed physical identity, possibly without a mounted codec.
 * @param lba First physical sector. @param sectors Written sector count.
 * @return None. IRQ-safe under execution/optional transport lock, no allocation
 * or I/O. Journal-owned dirty data follows its existing cache ownership rules.
 */
static void ext_native_invalidate_write(const struct storage_volume *writer,uint64_t lba,uint32_t sectors)
{
    ext2_cache_invalidate_range(lba,sectors);
    storage_ext4_cache_invalidate_write(writer,lba,sectors);
}
/** @brief Change native ext allocation and refresh the retained inode size.
 * @param volume Borrowed selected/pinned instance. @param node Actual retained file.
 * @param mode Validated Linux fallocate mode. @param offset Range start.
 * @param len Range length.
 * @return Zero or native validation/allocation/I/O errno. Caller owns execution
 * and authorization; invalidates data cache after successful native mutation,
 * no graph/module lock spans provider I/O and failures remain observable.
 */
static int ext_native_fallocate(struct storage_volume *volume,struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t len)
{
    struct ext4_inode_view in;int ret=0;
    if (!ret) ret=storage_ext4_check_node(volume,node,&in);
    if (!ret && (in.mode&0170000)!=0100000) ret=-RELIEFOS_EINVAL;
    if (!ret) ret=storage_ext4_fallocate(volume,node->first_cluster,mode,offset,len);
    if (!ret) {
        page_cache_invalidate_node(node); storage_cache_invalidate();
        ret=storage_ext4_read_inode(volume,node->first_cluster,&in);
        if (!ret) node->size=in.size;
    }
    return ret;
}
const struct storage_filesystem_ops storage_ext_ops={
    .fallocate=ext_native_fallocate,
    .sync=storage_ext4_sync,
    .invalidate_write=ext_native_invalidate_write,
    .create=ext_native_create,
    .remove=ext_native_remove,
    .rename=ext_native_rename,
    .link=ext_native_link,
    .hold_inode=ext_native_hold_inode,
    .put_inode=ext_native_put_inode,
    .write=ext_native_write,
    .truncate=ext_native_truncate,
    .statfs=ext_native_statfs,
    .name="ext4",
    .owner=THIS_MODULE,
    .mount=ext_native_mount,
    .destroy=ext_native_destroy,
    .permissions=ext_native_permissions,
    .stat=ext_native_stat,
    .utimens=ext_native_utimens,
    .read=ext_native_read,
    .readdir=ext_native_readdir,
    .root=ext_native_root,
    .lookup=ext_native_lookup,
    .readlink=ext_native_readlink
};


EXPORT_SYMBOL(storage_ext_ops);

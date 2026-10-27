/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"

/** @brief Describe the retained builtin memory filesystem root.
 * @param volume Borrowed pinned instance. @param out Required metadata output.
 * @return Zero or native errno. Execution ownership required; no name routing.
 */
static int tmpfs_native_root(struct storage_volume *volume,struct storage_node *out)
{ return tmpfs_lookup(volume->tmpfs,"/",out); }
/** @brief Look up one component in the actual builtin memory directory.
 * @param volume Borrowed pinned instance. @param parent Actual directory inode.
 * @param name Literal component. @param out Required metadata output.
 * @return Zero or native errno; execution ownership required, no provider lock.
 */
static int tmpfs_native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                                 const char *name,struct storage_node *out)
{ return tmpfs_lookup_child(volume->tmpfs,parent->first_cluster,name,out); }
/** @brief Copy the target of a retained builtin memory symlink.
 * @param volume Borrowed pinned instance. @param node Actual symlink inode.
 * @param bytes Required output. @param capacity Available bytes.
 * @param length Required actual byte count output.
 * @return Zero or native errno; execution ownership required, no I/O or routing.
 */
static int tmpfs_native_readlink(struct storage_volume *volume,const struct storage_node *node,
                                   char *bytes,uint32_t capacity,uint32_t *length)
{ return tmpfs_readlink(volume->tmpfs,node->first_cluster,bytes,capacity,length); }
/** @brief Release a quiesced builtin memory superblock after its last inode.
 * @param volume Exclusive instance with all mounts/inodes retired.
 * @return None. Execution ownership required, no I/O or provider locks.
 */
static void tmpfs_native_destroy(struct storage_volume *volume)
{ tmpfs_destroy(volume->tmpfs);volume->tmpfs=NULL; }
/** @brief Read the selected provider's actual retained file extent/stream.
 * @param volume Borrowed selected/pinned instance. @param node Retained file.
 * @param offset Byte position. @param bytes Validated writable destination.
 * @param capacity Available bytes, bounded by refreshed inode size.
 * @param length Optional actual-count output, initially zero.
 * @param cursor Optional caller-owned hint. @param mount_flags Access-local flags.
 * @return Zero or negative provider errno. Execution ownership required across
 * native I/O; no graph/module lock, code/instance lifetime held by caller.
 */
static int tmpfs_native_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                                  void *bytes,uint32_t capacity,uint32_t *length,
                                  struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    if(cursor)cursor->valid=0;
    return tmpfs_read_with_flags(volume->tmpfs,node->first_cluster,offset,bytes,capacity,length,mount_flags);
}
/** @brief Enumerate the selected provider's actual retained directory.
 * @param volume Borrowed selected/pinned instance. @param node Actual directory.
 * @param cursor Required opaque position. @param entry Required metadata output.
 * @param mount_flags Current attachment/shared flags for this access only.
 * @return One entry, zero EOF or negative native errno. Execution ownership
 * required; no graph/module lock spans native I/O, no retained output pointer.
 */
static int tmpfs_native_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                                     struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    return tmpfs_readdir_with_flags(volume->tmpfs,node->first_cluster,cursor,entry,mount_flags);
}
/** @brief Query/store authorized builtin memory inode ownership and mode.
 * @param volume Borrowed pinned instance. @param node Actual retained inode.
 * @param value Required result or authorized metadata. @param write Store if true.
 * @return Zero or native inode errno; execution ownership and prior permission
 * checks required, no I/O or graph/module lock.
 */
static int tmpfs_native_permissions(struct storage_volume *volume,const struct storage_node *node,
                                      struct reliefos_permissions *value,bool write)
{ return tmpfs_permissions(volume->tmpfs,node->first_cluster,value,write); }
/** @brief Report full metadata of the actual builtin memory inode.
 * @param volume Borrowed pinned instance. @param node Actual retained inode.
 * @param out Required caller-owned stat output, unspecified on error.
 * @return Zero or native inode errno; execution ownership required, no I/O.
 */
static int tmpfs_native_stat(struct storage_volume *volume,const struct storage_node *node,struct linux_stat_abi *out)
{ return tmpfs_stat(volume->tmpfs,node->first_cluster,out); }
/** @brief Store authorized times of the actual builtin memory inode.
 * @param volume Borrowed pinned instance. @param node Actual retained inode.
 * @param atime New access seconds. @param mtime New modification seconds.
 * @param set_atime Whether to store access time. @param set_mtime Whether to store modification time.
 * @return Zero or native inode errno; execution ownership/prior authorization
 * required, no I/O or graph/module lock.
 */
static int tmpfs_native_utimens(struct storage_volume *volume,const struct storage_node *node,int64_t atime,int64_t mtime,
                                  bool set_atime,bool set_mtime)
{ return tmpfs_utimens(volume->tmpfs,node->first_cluster,atime,mtime,set_atime,set_mtime); }
/** @brief Report capacity/identity from the selected native filesystem.
 * @param volume Borrowed selected/pinned instance. @param value Required output
 * initialized with generic flags/name limit/device identity by the caller.
 * @return Zero or native metadata/I/O errno. Execution ownership required;
 * no graph/module lock spans bitmap/group reads, output unspecified on error.
 */
static int tmpfs_native_statfs(struct storage_volume *volume,struct linux_statfs_abi *value)
{
    tmpfs_statfs(volume->tmpfs,value);return 0;
}
/** @brief Validate/retain one native inode for a new core inode reference.
 * @param volume Borrowed selected/pinned instance. @param node Actual inode.
 * @return Zero or native inode/allocation errno. Execution ownership required;
 * success pairs with put_inode on the final core reference, no callback lock.
 */
static int tmpfs_native_hold_inode(struct storage_volume *volume,const struct storage_node *node)
{
    return tmpfs_hold(volume->tmpfs,node->first_cluster);
}
/** @brief Retire native inode backing after the final core reference.
 * @param volume Borrowed selected/pinned instance. @param node Actual inode.
 * @param unlinked True if its directory identity was removed while held.
 * @return Zero or native cleanup errno retaining the core inode on failure.
 * Execution ownership required, synchronous non-replayable final cleanup;
 * no module/graph lock spans native I/O and no freed slot is used as identity.
 */
static int tmpfs_native_put_inode(struct storage_volume *volume,const struct storage_node *node,bool unlinked)
{
    (void)unlinked;tmpfs_drop(volume->tmpfs,node->first_cluster);return 0;
}
/** @brief Write an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param offset Byte position. @param bytes Validated source bytes.
 * @param length Source byte count. @param written Optional actual-count output.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int tmpfs_native_write(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
                                   const void *bytes,uint32_t length,uint32_t *written)
{
    return tmpfs_write(volume->tmpfs,node->first_cluster,offset,bytes,length,written);
}
/** @brief Resize an authorized retained native file after core writer admission.
 * @param volume Borrowed selected/pinned instance. @param node Actual mutable inode.
 * @param length Desired byte length.
 * @return Zero or native I/O/allocation errno. Execution ownership, write lease
 * and mutation admission required; no graph/module lock spans native I/O.
 */
static int tmpfs_native_truncate(struct storage_volume *volume,struct storage_node *node,uint64_t length)
{
    return tmpfs_truncate(volume->tmpfs,node->first_cluster,length);
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
static int tmpfs_native_create(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                                    const struct reliefos_permissions *value,const char *target,struct storage_node *out)
{
    return tmpfs_create_child(volume->tmpfs,parent->first_cluster,name,mode,target,value,out);
}
/** @brief Remove the exact verified native member, retaining open inode backing.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param node Verified victim metadata.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or native errno. Execution/mutation admission required; native
 * orphan/cache retirement precedes any final reuse, no callback lock across I/O.
 */
static int tmpfs_native_remove(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                                    const struct storage_node *node,bool directory)
{
    (void)node;return tmpfs_unlink_child(volume->tmpfs,parent->first_cluster,name,directory);
}
/** @brief Rename verified native members using the core's preheld graph operands.
 * @param volume Borrowed selected/pinned instance. @param args Required borrowed
 * operands and commit output, valid only during this synchronous callback.
 * @return Zero or native errno. Execution/mutation admission required; reports
 * irreversible commit even if later persistence fails, no callback lock.
 */
static int tmpfs_native_rename(struct storage_volume *volume,const struct storage_rename_args *args)
{
    int rc=tmpfs_rename_child(volume->tmpfs,args->old_parent->first_cluster,args->old_name,
        args->new_parent->first_cluster,args->new_name);
    *args->committed=!rc;return rc;
}
/** @brief Publish a hard link to the actual retained native inode.
 * @param volume Borrowed selected/pinned instance. @param source Actual inode.
 * @param parent Retained destination directory. @param name Validated component.
 * @return Zero or native errno. Caller checked permissions/link eligibility;
 * execution/mutation admission required, no graph/module lock across native I/O.
 */
static int tmpfs_native_link(struct storage_volume *volume,const struct storage_node *source,
                                  const struct storage_node *parent,const char *name)
{
    return tmpfs_link_child(volume->tmpfs,source->first_cluster,parent->first_cluster,name);
}
const struct storage_filesystem_ops storage_tmpfs_ops={
    .create=tmpfs_native_create,
    .remove=tmpfs_native_remove,
    .rename=tmpfs_native_rename,
    .link=tmpfs_native_link,
    .hold_inode=tmpfs_native_hold_inode,
    .put_inode=tmpfs_native_put_inode,
    .write=tmpfs_native_write,
    .truncate=tmpfs_native_truncate,
    .statfs=tmpfs_native_statfs,
    .name="tmpfs",
    .destroy=tmpfs_native_destroy,
    .permissions=tmpfs_native_permissions,
    .stat=tmpfs_native_stat,
    .utimens=tmpfs_native_utimens,
    .read=tmpfs_native_read,
    .readdir=tmpfs_native_readdir,
    .root=tmpfs_native_root,
    .lookup=tmpfs_native_lookup,
    .readlink=tmpfs_native_readlink
};

/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* This unpublished view delegates all path traversal to ordinary VFS namei.
 * It only borrows the constructor's tmpfs; runtime mounts use the existing
 * storage backend, and no special root-prefix routing survives construction. */
/** @brief Read one actual directory component in the private memory root.
 * @param context Borrowed tmpfs. @param parent Held directory inode.
 * @param name Literal component. @param out Required borrowed metadata result.
 * @return Native lookup errno or zero; exclusive construction, no I/O/locks.
 */
static int view_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{return tmpfs_lookup_child(context,parent->first_cluster,name,out);}
/** @brief Pin an actual memory inode while its VFS dentry remains retained.
 * @param context Borrowed tmpfs. @param node Actual inode metadata.
 * @param pin Required token output; inode identity remains in node.
 * @return Zero or native inode errno; caller owns construction execution.
 * No allocation, I/O or module locks; success pairs with view_release.
 */
static int view_retain(void *context,const struct storage_node *node,void **pin)
{*pin=NULL;return tmpfs_hold(context,node->first_cluster);}
/** @brief Release one namei dentry's memory-inode hold.
 * @param context Borrowed live tmpfs. @param node Previously retained inode.
 * @param pin Unused NULL token, never dereferenced.
 * @return None. Serialized construction only, no I/O or namespace mutation.
 */
static void view_release(void *context,const struct storage_node *node,void *pin)
{(void)pin;tmpfs_drop(context,node->first_cluster);}
/** @brief Compare monotonically assigned inode identities in this one tmpfs.
 * @param context Borrowed tmpfs, unused. @param a First borrowed metadata.
 * @param b Second borrowed metadata.
 * @return True for the same inode. Pure construction callback, no locks/I/O.
 */
static bool view_same(void *context,const struct storage_node *a,const struct storage_node *b)
{(void)context;return a->first_cluster==b->first_cluster;}
/** @brief Read a memory symlink into the ordinary name walker's scratch space.
 * @param context Borrowed tmpfs. @param node Held symlink inode.
 * @param bytes Required output. @param capacity Available destination bytes.
 * @param length Required actual byte output.
 * @return Zero or native input/type/inode errno; no ownership transfer/I/O.
 * Serialized construction keeps the inode and callback code alive.
 */
static int view_readlink(void *context,const struct storage_node *node,char *bytes,size_t capacity,size_t *length)
{
    uint32_t done=0;int rc=tmpfs_readlink(context,node->first_cluster,bytes,(uint32_t)capacity,&done);
    *length=done;return rc;
}
/** @brief Assemble a private VFS root without taking tmpfs backing ownership.
 * @param context Exclusive zero-initialized construction state with live fs.
 * @return Zero or lookup/allocation errno; no leaked mount/path on failure.
 * Exclusive boot execution required; no global publication, disk I/O or locks.
 */
int initramfs_view_start(struct unpack_context *context)
{
    static const struct vfs_backend_ops ops={.lookup=view_lookup,.retain=view_retain,
        .release=view_release,.same_node=view_same,.readlink=view_readlink};
    struct storage_node root;struct vfs_mount *mount=NULL;
    int rc=tmpfs_lookup(context->fs,"/",&root);
    if(!rc)rc=vfs_mount_new(&ops,context->fs,NULL,&root,&mount);
    if(!rc)rc=vfs_namespace_init(&context->ns,mount);
    vfs_mount_put(mount);
    if(!rc)rc=vfs_namespace_root(&context->ns,&context->root);
    if(rc && context->ns.root)(void)initramfs_view_stop(context);
    return rc;
}
/** @brief Retire all private namei paths before backing publication/destruction.
 * @param context Exclusive state, no active callbacks or retained operands.
 * @return Zero or EBUSY if construction leaked a path. No backing ownership
 * transfer; root dentry retirement drops inode holds without disk I/O/locks.
 */
int initramfs_view_stop(struct unpack_context *context)
{vfs_path_put(&context->root);return vfs_namespace_destroy(&context->ns);}

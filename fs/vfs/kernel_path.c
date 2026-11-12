/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_path_internal.h"

#include <reliefnt/permissions.h>
#include <linux/stat.h>

/** @brief Resolve one absolute kernel name with the actual shared graph.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel input.
 * @param flags Supported common walker flags. @param out Empty owned output.
 * @return Zero or negative errno, empty result on error. Caller holds execution
 * ownership; synchronous storage scope and every intermediate pin are balanced.
 * No graph/module lock spans callbacks; no credential check is implied.
 */
int vfs_kernel_lookup(struct vfs_namespace *ns,const char *name,unsigned flags,struct vfs_lookup_result *out)
{
    if(!out)return -EINVAL;*out=(struct vfs_lookup_result){0};
    if(!ns)return -ENODEV;
    if(!name || name[0]!='/' || flags&~(VFS_LOOKUP_FOLLOW|VFS_LOOKUP_PARENT|VFS_LOOKUP_MISSING))return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();
    struct vfs_path root={0};const struct vfs_lookup_policy policy={.flags=flags};
    int rc=vfs_namespace_root(ns,&root);
    if(!rc)rc=vfs_path_resolve(ns,&root,&root,name,&policy,out);
    vfs_path_put(&root);vfs_kernel_io_end(scope);return rc;
}

/** @brief Create exactly one missing final directory with explicit metadata.
 * @param ns Borrowed namespace. @param name Trusted absolute pathname.
 * @param mode Permission bits only, without task umask.
 * @return Zero or negative errno, preserving existing entries. Execution
 * ownership required; scope and path ownership balance every failure. No
 * graph/module lock spans lookup or inode-relative creation callbacks.
 */
int vfs_kernel_mkdir(struct vfs_namespace *ns,const char *name,uint32_t mode)
{
    if(mode&~07777u)return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();struct vfs_lookup_result found={0};
    int rc=vfs_kernel_lookup(ns,name,VFS_LOOKUP_MISSING,&found);
    struct reliefos_permissions value={mode,0,0};
    if(!rc)rc=found.missing ? vfs_path_create_node(&found.path,found.last,LINUX_S_IFDIR,&value,NULL,NULL) : -EEXIST;
    vfs_path_put(&found.path);vfs_kernel_io_end(scope);return rc;
}

/** @brief Replace bytes on an already resolved existing or missing member.
 * @param found Owned lookup result; updated in place when creation succeeds.
 * @param buffer Kernel bytes, NULL at zero length. @param length Exact byte count.
 * @return Zero or negative errno; a post-truncate failure can leave a prefix.
 * Actual path pins filesystem code; native inode-wide writer admission rejects
 * executing files before truncation. Execution ownership/synchronous storage
 * required; writer/inode references always retire without callback locks.
 */
static int kernel_replace_file(struct vfs_lookup_result *found,const void *buffer,uint32_t length)
{
    int rc=0;
    if(found->missing) {
        if(found->trailing)return -EISDIR;
        struct reliefos_permissions value={0644,0,0};struct vfs_path created={0};
        rc=vfs_path_new_file(&found->path,found->last,&value,&created);
        if(!rc){vfs_path_put(&found->path);found->path=created;found->missing=false;}
    }
    const struct storage_node *node=rc ? NULL : vfs_path_node(&found->path);
    if(!rc && node->type!=RELIEFOS_FS_TYPE_FILE)rc=node->type==RELIEFOS_FS_TYPE_DIR ? -EISDIR : -EINVAL;
    if(!rc && (vfs_path_mount_flags(&found->path)&MS_RDONLY))rc=-EROFS;
    struct storage_inode_ref *inode=NULL;bool writer=false;
    if(!rc)rc=storage_inode_get(node,&inode);
    if(!rc && !inode)rc=-EOPNOTSUPP;
    if(!rc){rc=storage_inode_write_get(inode);writer=!rc;}
    if(!rc)rc=vfs_path_truncate(&found->path,0);
    uint32_t done=0;
    while(!rc && done<length) {
        uint32_t remaining=length-done,take=remaining>INT32_MAX ? INT32_MAX : remaining,got=0;
        rc=vfs_path_write(&found->path,done,(const char *)buffer+done,take,&got);
        if(!rc && !got)rc=-EIO;
        done+=got;
    }
    if(writer)storage_inode_write_put(inode);
    int release=storage_inode_put(inode);return rc ? rc : release;
}

/** @brief Replace trusted kernel data with one actual lookup and writer lease.
 * @param ns Borrowed namespace. @param name Trusted absolute pathname.
 * @param buffer Kernel bytes, NULL only at length0. @param length Exact byte count.
 * @return Zero or negative errno. New0644 root metadata, existing metadata
 * preserved. Caller authorizes this kernel operation under execution ownership;
 * restores outer storage state, releases all paths/leases and holds no graph
 * or module lock across callbacks. Post-truncate failures may leave a prefix.
 */
int vfs_kernel_write_file(struct vfs_namespace *ns,const char *name,const void *buffer,uint32_t length)
{
    if(!buffer && length)return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();struct vfs_lookup_result found={0};
    int rc=vfs_kernel_lookup(ns,name,VFS_LOOKUP_FOLLOW|VFS_LOOKUP_MISSING,&found);
    if(!rc)rc=kernel_replace_file(&found,buffer,length);
    vfs_path_put(&found.path);vfs_kernel_io_end(scope);return rc;
}

/** @brief Remove a trusted visible file/link member through held actual objects.
 * @param ns Borrowed namespace. @param name Trusted absolute pathname.
 * @return Zero or negative errno. Final symlinks remain literal victims.
 * Caller authorizes parent policy under execution ownership; restores outer
 * storage state and releases all pins without graph/module callback locks.
 */
int vfs_kernel_unlink(struct vfs_namespace *ns,const char *name)
{
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();struct vfs_lookup_result found={0};
    int rc=vfs_kernel_lookup(ns,name,0,&found);
    if(!rc)rc=vfs_path_remove(ns,&found.path,false);
    vfs_path_put(&found.path);vfs_kernel_io_end(scope);return rc;
}

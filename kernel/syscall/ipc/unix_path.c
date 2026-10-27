/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include "unix_path.h"

/** @brief Create or authorize a pathname UNIX endpoint in the held mount tree.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable filesystem paths and credentials.
 * @param name Kernel-owned nonempty pathname, captured by the name adapter.
 * @param create True for bind creation, false for peer lookup.
 * @param out Empty output owning one actual inode path on success.
 * @return Zero or negative errno, no reference on error. Creation honors
 * umask/SGID/EADDRINUSE, peers require socket type and write DAC. Caller puts
 * the result; no graph/module/scheduler lock spans provider callbacks.
 */
int syscall_unix_path(struct vfs_namespace *ns,struct task *task,const char *name,
                          bool create,struct vfs_path *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct vfs_path){0};
    int rc;
    struct vfs_lookup_result found={0};
    rc=resolve_kernel_object_at_flags(ns,task,LINUX_AT_FDCWD,name,false,false,
        create ? VFS_LOOKUP_PARENT : VFS_LOOKUP_FOLLOW,&found);
    if(rc)return rc;
    if(create) {
        if(!found.last[0] || !__builtin_strcmp(found.last,".") || !__builtin_strcmp(found.last,".."))rc=-RELIEFOS_EADDRINUSE;
        struct vfs_path existing={0};
        if(!rc) {
            rc=vfs_path_lookup_child(&found.path,found.last,&existing);
            if(!rc)rc=-RELIEFOS_EADDRINUSE;
            else if(rc==-RELIEFOS_ENOENT)rc=found.trailing ? -RELIEFOS_ENOENT : 0;
        }
        vfs_path_put(&existing);
        struct reliefos_permissions mode;
        if(!rc)rc=syscall_creation_mode(task,&found.path,LINUX_S_IFSOCK,0777,&mode);
        if(!rc)rc=vfs_path_create_node(&found.path,found.last,LINUX_S_IFSOCK,&mode,NULL,out);
        if(rc==-RELIEFOS_EEXIST)rc=-RELIEFOS_EADDRINUSE;
    } else {
        rc=fs_permissions_check_path(task,&found.path,FS_ACCESS_WRITE,false);
        if(!rc && vfs_path_node(&found.path)->type!=RELIEFOS_FS_TYPE_SOCKET)rc=-LINUX_ECONNREFUSED;
        if(!rc){*out=found.path;found.path=(struct vfs_path){0};}
    }
    vfs_path_put(&found.path);
    return rc;
}

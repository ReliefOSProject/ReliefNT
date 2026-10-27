/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/procfs.h>
#include <reliefnt/inventory.h>

/** @brief Resolve both exact endpoints of a mount move and commit the graph.
 * @param ns Borrowed execution-owned namespace.
 * @param task Authorized caller with actual root/cwd and optional retry scope.
 * @param source User source pathname pointer, required.
 * @param target User destination pathname pointer, required.
 * @return Zero or lookup/type/cycle/busy errno; EAGAIN preserves scoped
 * operands. Local references retire on every path; no graph lock spans I/O.
 */
static int mount_move(struct vfs_namespace *ns,struct task *task,uint64_t source,uint64_t target)
{
    struct vfs_lookup_result from={0},to={0};
    int rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,source,false,false,VFS_LOOKUP_FOLLOW,&from);
    if(!rc)rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,target,false,false,VFS_LOOKUP_FOLLOW,&to);
    if(!rc)rc=vfs_mount_move(ns,&from.path,&to.path);
    vfs_path_put(&from.path);vfs_path_put(&to.path);return rc;
}

/** @brief Sync and detach the selected topmost mount with real busy checking.
 * @param ns Borrowed execution-owned namespace.
 * @param task Authorized caller retaining any retry scope.
 * @param target User target pathname pointer, required.
 * @param flags Low-32-bit Linux umount flags; nofollow is supported.
 * @return Zero or input/unsupported/lookup/busy/provider errno. Lazy/forced/
 * expired unmount is explicitly unsupported. A sync wait keeps the original
 * scope; after sync its redundant reference is retired before graph detach.
 * The local path pins code/backing until all callbacks and cleanup finish.
 */
static int mount_unmount(struct vfs_namespace *ns,struct task *task,uint64_t target,uint32_t flags)
{
    if(flags&~(MNT_FORCE|MNT_DETACH|MNT_EXPIRE|UMOUNT_NOFOLLOW))return -RELIEFOS_EINVAL;
    if(flags&(MNT_FORCE|MNT_DETACH|MNT_EXPIRE))return -RELIEFOS_EOPNOTSUPP;
    struct vfs_lookup_result found={0};
    int rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,target,false,false,
        flags&UMOUNT_NOFOLLOW ? 0 : VFS_LOOKUP_FOLLOW,&found);
    if(!rc) {
        rc=vfs_path_sync(&found.path,true,false);
    }
    if(!rc) {
        task_lookup_scope_cancel(task);
        rc=vfs_mount_unmount(ns,&found.path);
    }
    vfs_path_put(&found.path);return rc;
}

/** @brief Validate and apply policy to the actual selected filesystem root.
 * @param ns Borrowed execution-owned namespace.
 * @param task Authorized calling task with retry-stable lookup scope.
 * @param target User target pathname pointer, required.
 * @param flags Final requested policy with MS_REMOUNT already stripped.
 * @return Zero or lookup/type/unsupported/busy/provider errno. Linux retains
 * existing atime policy when remount supplies none of its controlling bits.
 * Local path owns provider lifetime; no graph/module lock spans the callback.
 */
static int mount_remount(struct vfs_namespace *ns,struct task *task,uint64_t target,uint64_t flags)
{
    struct vfs_lookup_result found={0};
    int rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,target,false,false,VFS_LOOKUP_FOLLOW,&found);
    if(!rc) {
        const uint64_t atime=MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME;
        if(!(flags&atime))flags|=vfs_path_mount_flags(&found.path)&atime;
        rc=vfs_path_remount(&found.path,flags);
    }
    vfs_path_put(&found.path);return rc;
}

/** @brief Construct one supported backend without publishing a pathname alias.
 * @param ns Borrowed execution-owned namespace for block-source resolution.
 * @param task Authorized caller whose fsuid/fsgid own a new tmpfs root.
 * @param source Copied kernel source label; its user pointer is passed separately.
 * @param source_user Original user source pathname for a disk filesystem.
 * @param type Copied filesystem type, empty for existing native detection.
 * @param options Copied option string; only tmpfs currently consumes options.
 * @param flags Final mount policy without operation bits.
 * @param out Receives one owned detached mount, empty on failure.
 * @return Zero or unsupported/device/lookup/provider errno. Source resolution
 * retains its actual standard block node across waits; no name parsing to
 * choose a disk. Disk metadata I/O is synchronous and no graph lock is held.
 */
static int mount_backend(struct vfs_namespace *ns,struct task *task,const char *source,uint64_t source_user,
                             const char *type,const char *options,uint64_t flags,struct vfs_mount **out)
{
    if(!__builtin_strcmp(type,"tmpfs"))return storage_new_tmpfs_mount(source,flags,options,task->fsuid,task->fsgid,out);
    if(options[0])return -RELIEFOS_EOPNOTSUPP;
    int rc;
    if(!__builtin_strcmp(type,"proc"))rc=proc_create_vfs_mount(out);
    else if(!__builtin_strcmp(type,"sysfs"))rc=sysfs_create_vfs_mount(out);
    else if(!__builtin_strcmp(type,"devfs"))rc=devfs_create_vfs_mount(out);
    else {
        struct vfs_lookup_result found={0};
        rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,source_user,false,false,VFS_LOOKUP_FOLLOW,&found);
        if(!rc) {
            const struct storage_node *node=vfs_path_node(&found.path);
            struct storage_node device;
            if(node->flags&STORAGE_NODE_FLAG_RDEV) {
                struct linux_stat_abi status;rc=vfs_path_stat(&found.path,&status);
                if(!rc && (status.st_mode&LINUX_S_IFMT)!=LINUX_S_IFBLK)rc=-LINUX_ENOTBLK;
                if(!rc)rc=devfs_lookup_rdev(LINUX_S_IFBLK,status.st_rdev,&device);
                if(!rc)node=&device;
            }
            if(!rc && !(node->flags&STORAGE_NODE_FLAG_DEV_BLOCK))rc=-LINUX_ENOTBLK;
            if(!rc) {
                int32_t partition=STORAGE_BLOCK_PARTITION(node->volume_id);
                rc=storage_new_block_mount(STORAGE_BLOCK_DISK_ID(node->volume_id),
                    (uint32_t)partition,type[0] ? type : NULL,flags,out);
            }
        }
        vfs_path_put(&found.path);return rc;
    }
    /* Generated providers can carry independent attachment restrictions.
     * They expose no mutable shared sync/dirsync/lazytime backend policy. */
    if(!rc && (flags&(VFS_SUPER_POLICY_MASK&~MS_RDONLY))) {
        rc=-RELIEFOS_EOPNOTSUPP;
        if(rc){vfs_mount_put(*out);*out=NULL;}
    }
    return rc;
}

/** @brief Apply a Linux mount operation using retry-stable actual objects.
 * @param ns Borrowed live namespace under execution ownership.
 * @param task Calling task, required; CAP_SYS_ADMIN checked before user input.
 * @param number Linux MOUNT or UMOUNT2 operation number.
 * @param a0 User source/label, or unmount target pointer.
 * @param a1 Mount target pointer, or unmount low-32-bit flags.
 * @param a2 User filesystem type; ignored for remount/move/unmount.
 * @param a3 Native mount flags; ignored for unmount. Legacy magic and the
 * MS_SILENT diagnostic hint never become attachment policy or move selectors.
 * @param a4 Optional user mount data; ignored for unmount.
 * @return Zero or negative Linux errno/internal EAGAIN. Text and lookup state
 * remain scope-owned across waits; temporary unscoped allocations are always
 * freed. The resulting graph owns backend lifetime, without target prefixes.
 */
int64_t syscall_mount_control(struct vfs_namespace *ns,struct task *task,uint64_t number,
                                 uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    if(!task || !(task->cap_effective&(1ULL<<CAP_SYS_ADMIN)))return -RELIEFOS_EPERM;
    if(!ns)return -RELIEFOS_ENOENT;
    if(number==LINUX_SYS_UMOUNT2)return mount_unmount(ns,task,a0,(uint32_t)a1);
    if(number!=LINUX_SYS_MOUNT)return -RELIEFOS_ENOSYS;
    if((a3&MS_MGC_MSK)==MS_MGC_VAL)a3&=~MS_MGC_MSK;
    a3&=~MS_SILENT;
    if(a3&MS_REMOUNT)return a4 ? -RELIEFOS_EOPNOTSUPP : mount_remount(ns,task,a1,a3&~MS_REMOUNT);
    if(a3&MS_MOVE)return a3!=MS_MOVE ? -RELIEFOS_EOPNOTSUPP : mount_move(ns,task,a0,a1);
    const uint64_t supported=MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_DIRSYNC|
        MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME|MS_LAZYTIME;
    if(a3&~supported)return -RELIEFOS_EOPNOTSUPP;
    struct task_user_text type={0},source={0},options={0};struct vfs_lookup_result target={0};
    struct vfs_mount *mount=NULL;
    int rc=task_lookup_user_text(task,a2,VFS_NAME_MAX+1,true,&type);
    if(!rc)rc=task_lookup_user_text(task,a0,VFS_PATH_MAX,true,&source);
    if(!rc)rc=task_lookup_user_text(task,a4,VFS_PATH_MAX,true,&options);
    if(!rc)rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,a1,false,false,VFS_LOOKUP_FOLLOW,&target);
    if(!rc && vfs_path_node(&target.path)->type!=RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_ENOTDIR;
    uint64_t flags=a3;
    if(!(flags&MS_NOATIME))flags|=MS_RELATIME;
    if(flags&MS_STRICTATIME)flags&=~(MS_NOATIME|MS_RELATIME);
    storage_set_io_async_context(false);
    if(!rc)rc=mount_backend(ns,task,source.text,a0,type.text,options.text,flags,&mount);
    if(!rc)rc=vfs_mount_set_flags(mount,flags&(VFS_MOUNT_POLICY_MASK|MS_STRICTATIME));
    if(!rc)rc=vfs_mount_attach(ns,mount,&target.path);
    vfs_mount_put(mount);vfs_path_put(&target.path);
    kernel_free(type.allocation);kernel_free(source.allocation);kernel_free(options.allocation);return rc;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_path_internal.h"

/* Exact boot-policy identity, never a pathname router or a borrowed mount/code
 * pointer. Registered dev_t values are not recycled after driver removal. */
static uint64_t early_boot_esp_device;

/** @brief Retain the actual root-mounted ESP identity selected by early PID1.
 * @return Zero or namespace/lookup/type/ENODEV/EBUSY errno. Exclusive boot
 * execution only; native FAT and a registered non-memory block device are
 * required. Failed binding changes nothing and all temporary path pins retire.
 * No graph/module lock spans native lookup; later operations resolve afresh.
 */
int storage_bind_boot_esp(void)
{
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();
    struct vfs_namespace *ns=vfs_system_namespace();struct vfs_lookup_result boot={0},root={0};
    int rc=vfs_kernel_lookup(ns,"/boot",VFS_LOOKUP_FOLLOW,&boot);
    if(!rc)rc=vfs_kernel_lookup(ns,"/",VFS_LOOKUP_FOLLOW,&root);
    const struct storage_node *node=!rc ? vfs_path_node(&boot.path) : NULL;
    struct storage_volume *volume=node && node->volume_id<STORAGE_MAX_VOLUMES ? &g_volumes[node->volume_id] : NULL;
    if(!rc && (!volume || vfs_path_context(&boot.path)!=volume || boot.path.mount==root.path.mount ||
        !volume->ready || volume->filesystem!=STORAGE_FILESYSTEM_FAT32 || !volume->device_ref.device ||
        volume->transport==STORAGE_TRANSPORT_MEMORY))rc=-ENODEV;
    uint64_t device=!rc ? storage_volume_device_number(volume) : 0;
    if(!rc && !device)rc=-ENODEV;
    if(!rc && early_boot_esp_device && early_boot_esp_device!=device)rc=-EBUSY;
    if(!rc)early_boot_esp_device=device;
    vfs_path_put(&root.path);vfs_path_put(&boot.path);vfs_kernel_io_end(scope);return rc;
}
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

/** @brief Ensure a literal boot-control directory stays on the actual ESP mount.
 * @param ns Borrowed system namespace. @param boot Held visible ESP root.
 * @param parent Held directory on that attachment. @param name Constant child.
 * @param out Required empty owned path, released on failure.
 * @return Zero or negative lookup/type/create/cross-attachment errno. Existing
 * metadata is preserved; new directories are root-owned0755. Execution-owned
 * synchronous callbacks run without graph/module locks; no links are followed.
 */
static int kernel_boot_directory(struct vfs_namespace *ns,const struct vfs_path *boot,
                                    const struct vfs_path *parent,const char *name,struct vfs_path *out)
{
    int rc=vfs_path_lookup(ns,boot,parent,name,0,out);
    if(rc==-ENOENT) {
        struct reliefos_permissions value={0755,0,0};
        rc=vfs_path_create_node(parent,name,LINUX_S_IFDIR,&value,NULL,out);
    }
    if(!rc && out->mount!=boot->mount)rc=-EXDEV;
    if(!rc && vfs_path_node(out)->type!=RELIEFOS_FS_TYPE_DIR)rc=-ENOTDIR;
    if(rc)vfs_path_put(out);return rc;
}

/** @brief Resolve a control member only on the boot volume's visible attachment.
 * @param name Absolute /boot/ descendant; validated without text-based routing.
 * @param flags Common walker flags for creation or literal unlink.
 * @param prepare Ensure standard reliefos/state directories before a write.
 * @param out Required empty owned lookup result, released/empty on failure.
 * @return Zero or negative validation/lookup/ENODEV/EXDEV/create errno. The
 * current visible /boot must belong to the actual boot volume; an overmount,
 * unmount or final symlink escape cannot redirect mutation into another mount.
 * Execution ownership and synchronous storage required, no callback locks.
 */
static int kernel_boot_target(const char *name,unsigned flags,bool prepare,struct vfs_lookup_result *out)
{
    static const char prefix[]="/boot/";
    if(!name)return -EINVAL;
    for(unsigned i=0;i<sizeof(prefix)-1;++i)if(name[i]!=prefix[i])return -EINVAL;
    struct vfs_namespace *ns=vfs_system_namespace();struct vfs_lookup_result boot={0};
    struct vfs_path directory={0},state={0};int rc=vfs_kernel_lookup(ns,"/boot",VFS_LOOKUP_FOLLOW,&boot);
    if(rc==-ENOENT)rc=-ENODEV;
    if(!rc && early_boot_esp_device) {
        const struct storage_node *node=vfs_path_node(&boot.path);
        const struct storage_volume *volume=node && node->volume_id<STORAGE_MAX_VOLUMES ? &g_volumes[node->volume_id] : NULL;
        if(!volume || vfs_path_context(&boot.path)!=volume || !volume->ready ||
           volume->filesystem!=STORAGE_FILESYSTEM_FAT32 || storage_volume_device_number(volume)!=early_boot_esp_device)rc=-ENODEV;
    } else if(!rc && (!g_volumes[STORAGE_VOLUME_BOOT].ready ||
        vfs_path_context(&boot.path)!=&g_volumes[STORAGE_VOLUME_BOOT] ||
        vfs_path_node(&boot.path)->volume_id!=STORAGE_VOLUME_BOOT))rc=-ENODEV;
    if(!rc && prepare)rc=kernel_boot_directory(ns,&boot.path,&boot.path,"reliefos",&directory);
    if(!rc && prepare)rc=kernel_boot_directory(ns,&boot.path,&directory,"state",&state);
    if(!rc)rc=vfs_kernel_lookup(ns,name,flags,out);
    if(!rc && out->path.mount!=boot.path.mount)rc=-EXDEV;
    vfs_path_put(&state);vfs_path_put(&directory);vfs_path_put(&boot.path);
    if(rc)vfs_path_put(&out->path);return rc;
}

/** @brief Write a boot control file through the actual held ESP attachment.
 * @param path Trusted absolute /boot/ descendant, required.
 * @param buf Kernel bytes, NULL only at length0. @param len Complete byte count.
 * @return Zero or negative validation/attachment/lookup/writer/I/O errno. New
 * standard state directories are0755, new files0644, existing metadata stays.
 * Execution ownership required; no path replay after admission, no graph/module
 * lock spans callbacks. Storage async state and mutation latch are preserved;
 * all owned paths/inode leases retire before return. Partial writes may remain.
 */
int storage_write_boot_esp_file(const char *path,const void *buf,uint32_t len)
{
    if(!buf && len)return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();struct vfs_lookup_result found={0};
    int rc=kernel_boot_target(path,VFS_LOOKUP_FOLLOW|VFS_LOOKUP_MISSING,true,&found);
    if(!rc)rc=kernel_replace_file(&found,buf,len);
    vfs_path_put(&found.path);vfs_kernel_io_end(scope);return rc;
}

/** @brief Remove one literal boot-control entry from the actual ESP attachment.
 * @param path Trusted absolute /boot/ descendant, required.
 * @return Zero including an absent member, otherwise negative validation/
 * attachment/lookup/mutation errno. ENODEV never becomes absence-success.
 * Final symlinks are unlinked themselves. Execution ownership required, storage
 * scope and all provider pins balanced; no graph/module callback locks.
 */
int storage_unlink_boot_esp_file(const char *path)
{
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();struct vfs_lookup_result found={0};
    int rc=kernel_boot_target(path,0,false,&found);
    if(!rc)rc=vfs_path_remove(vfs_system_namespace(),&found.path,false);
    vfs_path_put(&found.path);vfs_kernel_io_end(scope);return rc==-ENOENT ? 0 : rc;
}

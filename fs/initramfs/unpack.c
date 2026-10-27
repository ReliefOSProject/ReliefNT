/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Encode a kernel device number without truncating newc fields.
 * @param entry Borrowed device record. @param out Required stat-rdev output.
 * @return Zero or EOVERFLOW beyond the kernel's 12:20-bit device space. Pure,
 * no allocation, ownership transfer, device binding, locks or I/O.
 */
static int unpack_device(const struct initramfs_entry *entry,uint64_t *out)
{
    uint64_t major=entry->rdevice_major,minor=entry->rdevice_minor;
    if(major>0xfff || minor>0xfffff)return -EOVERFLOW;
    *out=(major<<8)|(minor&255)|((minor&~255ULL)<<12);return 0;
}

/** @brief Materialize the exact final member beneath an actual directory.
 * @param context Exclusive construction state. @param entry Borrowed record.
 * @param parent Held parent. @param name Literal component.
 * @param link Existing tuple or NULL. @param target Copied symlink or NULL.
 * @param out Borrowed result metadata.
 * @return Zero or native type/create/link/remove errno. Directory replacement
 * requires emptiness; regular overwrites retain old hardlinks. No text replay,
 * module locks or disk I/O; failure destroys the whole unpublished root.
 */
static int unpack_member(struct unpack_context *context,const struct initramfs_entry *entry,
                            const struct vfs_path *parent,const char *name,struct unpack_link *link,
                            const char *target,struct storage_node *out)
{
    uint32_t parent_id=vfs_path_node(parent)->first_cluster,type=entry->mode&LINUX_S_IFMT;
    struct storage_node old;int rc=tmpfs_lookup_child(context->fs,parent_id,name,&old);
    bool exists=!rc;if(rc && rc!=-ENOENT)return rc;
    if(exists) {
        struct linux_stat_abi stat;rc=tmpfs_stat(context->fs,old.first_cluster,&stat);if(rc)return rc;
        if((!link && (stat.st_mode&LINUX_S_IFMT)==type && (type==LINUX_S_IFREG || type==LINUX_S_IFDIR)) ||
           (link && link->node==old.first_cluster)){*out=old;return 0;}
        rc=tmpfs_unlink_child(context->fs,parent_id,name,old.type==RELIEFOS_FS_TYPE_DIR);
        if(rc)return rc;
    }
    if(link) {
        rc=tmpfs_link_child(context->fs,link->node,parent_id,name);
        return rc ? rc : tmpfs_lookup_child(context->fs,parent_id,name,out);
    }
    struct reliefos_permissions value={entry->mode&07777,entry->uid,entry->gid};
    if(type==LINUX_S_IFCHR || type==LINUX_S_IFBLK) {
        uint64_t device;rc=unpack_device(entry,&device);
        return rc ? rc : tmpfs_mknod_child(context->fs,parent_id,name,entry->mode,device,&value,out);
    }
    return tmpfs_create_child(context->fs,parent_id,name,entry->mode,target,&value,out);
}

/** @brief Resolve one archive operand through ordinary rooted VFS namei.
 * @param context Exclusive state retaining private root and filesystem.
 * @param entry Borrowed record. @param link Existing tuple or NULL.
 * @param target Copied symlink or NULL. @param out Borrowed metadata result.
 * @return Zero or path/type/mutation errno. Dot-dot and absolute symlinks stay
 * inside the actual private root. All temporary paths retire before return;
 * no global publication, provider locks or disk I/O occurs.
 */
static int unpack_path(struct unpack_context *context,const struct initramfs_entry *entry,
                          struct unpack_link *link,const char *target,struct storage_node *out)
{
    if(!entry->name_length)return -ENOENT;
    struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_PARENT};
    struct vfs_lookup_result found={0};
    int rc=vfs_path_resolve(&context->ns,&context->root,&context->root,entry->name,&policy,&found);
    if(rc)return rc;
    if(found.trailing && (entry->mode&LINUX_S_IFMT)!=LINUX_S_IFDIR)rc=-ENOTDIR;
    if(!rc && (!found.last[0] || !__builtin_strcmp(found.last,".") || !__builtin_strcmp(found.last,".."))) {
        struct vfs_path node={0};
        rc=vfs_path_lookup(&context->ns,&context->root,&context->root,entry->name,VFS_LOOKUP_FOLLOW,&node);
        if(!rc && (entry->mode&LINUX_S_IFMT)!=LINUX_S_IFDIR)rc=-EISDIR;
        if(!rc)*out=*vfs_path_node(&node);
        vfs_path_put(&node);
    } else if(!rc)rc=unpack_member(context,entry,&found.path,found.last,link,target,out);
    vfs_path_put(&found.path);return rc;
}

/** @brief Construct one inode/link and store wire contents and ownership.
 * @param opaque Exclusive construction state, borrowed during scanning.
 * @param entry Borrowed record; NULL resets tuple state at TRAILER!!!.
 * @return Zero or negative construction errno. Per-entry paths/buffers retire
 * before return; ledger holds survive until trailer/final cleanup. A nonempty
 * later hardlink overwrites shared data; empty later links preserve contents.
 * No global publication, device callbacks, graph locks or disk I/O occurs.
 */
static int unpack_entry(void *opaque,const struct initramfs_entry *entry)
{
    struct unpack_context *context=opaque;
    if(!entry){initramfs_links_clear(context);return 0;}
    uint32_t type=entry->mode&LINUX_S_IFMT;
    struct unpack_link *link=type!=LINUX_S_IFDIR && entry->links>1 ? initramfs_link_find(context,entry) : NULL;
    if(link && link->type!=type)return -EINVAL;
    char *target=NULL;int rc=0;
    if(type==LINUX_S_IFLNK) {
        if(entry->size>=VFS_PATH_MAX)return -ENAMETOOLONG;
        for(uint32_t i=0;i<entry->size;++i)if(!entry->data[i])return -EINVAL;
        target=kernel_malloc((size_t)entry->size+1);if(!target)return -ENOMEM;
        __builtin_memcpy(target,entry->data,entry->size);target[entry->size]=0;
    }
    struct storage_node node;
    rc=unpack_path(context,entry,link,target,&node);
    if(!rc && type==LINUX_S_IFLNK && link)rc=tmpfs_rewrite_symlink(context->fs,node.first_cluster,target);
    kernel_free(target);
    if(!rc && type==LINUX_S_IFREG && (!link || entry->size)) {
        rc=tmpfs_truncate(context->fs,node.first_cluster,0);
        uint32_t written=0;
        if(!rc && entry->size) {
            rc=tmpfs_write(context->fs,node.first_cluster,0,entry->data,entry->size,&written);
            if(!rc && written!=entry->size)rc=-EIO;
        }
    }
    struct reliefos_permissions value={entry->mode&07777,entry->uid,entry->gid};
    if(!rc)rc=tmpfs_permissions(context->fs,node.first_cluster,&value,true);
    if(!rc)rc=tmpfs_utimens(context->fs,node.first_cluster,entry->mtime,entry->mtime,true,true);
    if(!rc && type==LINUX_S_IFDIR)rc=initramfs_directory_time(context,node.first_cluster,entry->mtime);
    if(!rc && type!=LINUX_S_IFDIR && entry->links>1 && !link)rc=initramfs_link_add(context,entry,&node);
    return rc;
}

/** @brief Transfer a memory root only after the entire archive is constructed.
 * @param image Stable readable length-byte stream, NULL permitted at length0.
 * @param length Complete newc extent. @param volume Reserved native identity.
 * @param out Required owned output, NULL on ordinary construction failures.
 * @return Zero or negative decode/path/type/device/allocation/quota errno.
 * All failed construction state is destroyed before return; bytes are copied
 * only into actual inode backing. Exclusive boot execution, no publication,
 * hardware callbacks, disk I/O or locks.
 */
int initramfs_unpack(const void *image,size_t length,uint32_t volume,struct tmpfs_super **out)
{
    if(!out)return -EINVAL;*out=NULL;
    struct unpack_context context={0};
    int rc=tmpfs_new(volume,0,"size=0,nr_inodes=0,mode=0755",0,0,&context.fs);
    if(!rc)rc=initramfs_view_start(&context);
    if(!rc)rc=initramfs_scan(image,length,unpack_entry,&context);
    initramfs_links_clear(&context);
    int times=initramfs_times_finish(&context,!rc);if(!rc)rc=times;
    int stop=context.ns.root ? initramfs_view_stop(&context) : 0;
    /* Construction callbacks retain no paths beyond their call. Never free
     * backing if a future callback violates that ownership contract. */
    if(stop)return stop;
    if(rc)tmpfs_destroy(context.fs);else *out=context.fs;
    return rc;
}

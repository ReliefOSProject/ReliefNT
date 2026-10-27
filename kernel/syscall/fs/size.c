/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Resize an authorized inode while excluding executable writers.
 * @param path Borrowed held inode retaining its provider through I/O.
 * @param length Validated nonnegative logical size.
 * @return Zero or negative errno. Acquires a temporary inode/write lease
 * before truncation and releases it on every outcome, including retry.
 * No text lookup or graph/module spinlock spans the provider callback.
 */
static int truncate_inode_path(const struct vfs_path *path,uint64_t length)
{
    const struct storage_node *node=vfs_path_node(path);
    if(!node)return -RELIEFOS_EINVAL;
    if(node->type==RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_EISDIR;
    if(node->type!=RELIEFOS_FS_TYPE_FILE)return -RELIEFOS_EINVAL;
    struct storage_inode_ref *inode=NULL;
    int rc=storage_inode_get(node,&inode);if(rc)return rc;
    if(!inode)return -RELIEFOS_EOPNOTSUPP;
    rc=storage_inode_write_get(inode);
    if(!rc) {
        rc=vfs_path_truncate(path,length);
        storage_inode_write_put(inode);
    }
    (void)storage_inode_put(inode);return rc;
}

/** @brief Implement truncate/ftruncate using retained inode objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with dispatcher-owned OFD admission.
 * @param number Linux TRUNCATE or FTRUNCATE number.
 * @param operand User pathname or signed descriptor respectively.
 * @param length Signed Linux logical length encoded in one register.
 * @return Zero or negative errno. Pathname calls check write DAC; descriptor
 * calls honor original open access and reject O_PATH. Provider callbacks
 * preserve shmem seals and filesystem readonly policy. Common retry state
 * retains the original inode/OFD without name replay or locks across I/O.
 */
int64_t syscall_truncate_object(struct vfs_namespace *ns,struct task *task,uint64_t number,
                                  uint64_t operand,uint64_t length)
{
    if((int64_t)length<0)return -RELIEFOS_EINVAL;
    if(!task)return -RELIEFOS_ESRCH;
    struct vfs_lookup_result found={0};struct task_file *file=NULL;int rc;
    if(number==LINUX_SYS_FTRUNCATE) {
        file=task_file_for_io(task,(int32_t)operand);
        if(!file || (file->flags&TASK_FILE_FLAG_PATH) || !file_can_write(file))return -RELIEFOS_EBADF;
        if(file->node.type!=RELIEFOS_FS_TYPE_FILE)return -RELIEFOS_EINVAL;
        if(!vfs_path_node(&file->path_ref))return -RELIEFOS_EINVAL;
        vfs_path_get(&file->path_ref,&found.path);rc=0;
    } else if(number==LINUX_SYS_TRUNCATE) {
        rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,operand,false,false,VFS_LOOKUP_FOLLOW,&found);
        if(!rc)rc=fs_permissions_check_path(task,&found.path,FS_ACCESS_WRITE,false);
    } else return -RELIEFOS_ENOSYS;
    if(!rc)rc=truncate_inode_path(&found.path,length);
    if(!rc && file) {
        file->node=*vfs_path_node(&found.path);file->read_cursor.valid=0;
    }
    vfs_path_put(&found.path);return rc;
}

/** @brief Preallocate through the original admitted writable description.
 * @param task Borrowed caller under execution ownership with OFD admission.
 * @param fd Signed user descriptor, never re-looked-up on a suspended retry.
 * @param mode Linux allocation flags as a 32-bit syscall int.
 * @param offset Signed starting offset encoded in a register.
 * @param length Positive signed byte count encoded in a register.
 * @return Zero or negative Linux errno; original access mode, inode writer
 * exclusion, provider seals and readonly policy remain authoritative. The
 * path pins callback code through allocation/I/O without graph/module locks.
 */
int64_t syscall_fallocate_object(struct task *task,int32_t fd,uint32_t mode,uint64_t offset,uint64_t length)
{
    struct task_file *file=task_file_for_io(task,fd);
    if(!file || (file->flags&TASK_FILE_FLAG_PATH) || !file_can_write(file))return -RELIEFOS_EBADF;
    if((int64_t)offset<0 || (int64_t)length<=0)return -RELIEFOS_EINVAL;
    if(length>(uint64_t)INT64_MAX-offset)return -RELIEFOS_EFBIG;
    if(mode&~(1u|2u|8u|16u|32u|64u))return -RELIEFOS_EOPNOTSUPP;
    if((mode&2u) && !(mode&1u))return -RELIEFOS_EOPNOTSUPP;
    if(((mode&2u) && (mode&~3u)) || ((mode&8u) && mode!=8u) ||
       ((mode&32u) && mode!=32u) || ((mode&64u) && (mode&~65u)))return -RELIEFOS_EINVAL;
    if(file->node.type==RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_EISDIR;
    if(file->node.type==RELIEFOS_FS_TYPE_FIFO || (file->flags&TASK_FILE_FLAG_PIPE))return -LINUX_ESPIPE;
    if(file->node.type!=RELIEFOS_FS_TYPE_FILE || !vfs_path_node(&file->path_ref))return -RELIEFOS_ENODEV;
    struct storage_inode_ref *inode=NULL;
    int rc=storage_inode_get(vfs_path_node(&file->path_ref),&inode);if(rc)return rc;
    if(!inode)return -RELIEFOS_EOPNOTSUPP;
    rc=storage_inode_write_get(inode);
    if(!rc) {
        rc=vfs_path_fallocate(&file->path_ref,mode,offset,length);
        storage_inode_write_put(inode);
    }
    (void)storage_inode_put(inode);
    if(!rc){file->node=*vfs_path_node(&file->path_ref);file->read_cursor.valid=0;}
    return rc;
}

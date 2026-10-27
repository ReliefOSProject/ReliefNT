/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/exec_file.h>
#include "mount_internal.h"

struct vfs_exec_source {
    struct vfs_path path;
    struct storage_node node;
    struct storage_inode_ref *inode;
    uint64_t references;
    bool denied_write;
};

int vfs_exec_source_open(const struct vfs_path *path,struct vfs_exec_source **out)
{
    if(!out)return -EINVAL;*out=NULL;
    const struct storage_node *node=vfs_path_node(path);
    if(!node || node->type!=RELIEFOS_FS_TYPE_FILE)return -EACCES;
    struct vfs_exec_source *source=kernel_malloc(sizeof(*source));if(!source)return -ENOMEM;
    *source=(struct vfs_exec_source){.node=*node,.references=1};vfs_path_get(path,&source->path);
    int rc=storage_inode_get(node,&source->inode);
    if(!rc && !source->inode)rc=-EOPNOTSUPP;
    if(!rc){rc=storage_inode_deny_write(source->inode);if(!rc)source->denied_write=true;}
    struct linux_stat_abi status;
    if(!rc)rc=vfs_path_stat(path,&status);
    if(!rc && status.st_size<0)rc=-EIO;
    if(!rc)source->node.size=(uint64_t)status.st_size;
    if(rc){vfs_exec_source_put(source);return rc;}
    *out=source;return 0;
}

struct vfs_exec_source *vfs_exec_source_get(struct vfs_exec_source *source)
{
    if(source && source->references!=UINT64_MAX)++source->references;
    return source;
}

void vfs_exec_source_put(struct vfs_exec_source *source)
{
    if(!source || source->references==UINT64_MAX || --source->references)return;
    if(source->denied_write)storage_inode_allow_write(source->inode);
    (void)storage_inode_put(source->inode);vfs_path_put(&source->path);kernel_free(source);
}

const struct storage_node *vfs_exec_source_node(const struct vfs_exec_source *source)
{return source ? &source->node : NULL;}

/** @brief Read an admitted executable inode with its retained attachment policy.
 * @param source Borrowed owned source retaining path/code and write exclusion.
 * @param offset Byte position. @param buffer Destination, NULL at capacity0.
 * @param capacity Available bytes. @param read Required actual count output.
 * @return Provider status or negative input/unsupported/count errno. Reads
 * stop at captured executable size. Execution ownership required; no allocation
 * or graph/module lock spans I/O and no formatted name is consulted.
 */
int vfs_exec_source_read(const struct vfs_exec_source *source,uint64_t offset,
                            void *buffer,uint32_t capacity,uint32_t *read)
{
    if(!read)return -EINVAL;*read=0;
    if(!source || (capacity && !buffer))return -EINVAL;
    if(offset>=source->node.size || !capacity)return 0;
    if(capacity>source->node.size-offset)capacity=(uint32_t)(source->node.size-offset);
    struct vfs_mount *mount=source->path.mount;
    if(!mount->ops->read)return -EOPNOTSUPP;
    int rc=mount->ops->read(mount->context,&source->node,offset,buffer,capacity,read,vfs_path_mount_flags(&source->path));
    if(*read>capacity){*read=0;return -EIO;}
    return rc;
}

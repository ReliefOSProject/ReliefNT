/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

struct vfs_open_file {struct vfs_path path;void *state;};

/** @brief Capture optional authorized per-open provider state transactionally.
 * @param path Borrowed actual inode, already authorized for sanitized flags.
 * @param flags Sanitized open status flags; caller bypasses this for O_PATH.
 * @param out Required owned output, NULL for ordinary stateless providers.
 * @return Zero or negative errno; failure leaves no state/path/provider pin.
 * The private capture callback must not block. A successful handle owns its
 * provider path until close, independently of the caller's path reference.
 * Execution ownership required; no graph/module lock spans callbacks.
 */
int vfs_file_open(const struct vfs_path *path,uint32_t flags,struct vfs_open_file **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!vfs_path_valid(path))return -EINVAL;
    const struct vfs_backend_ops *ops=path->mount->ops;
    if(!ops->file_open)return 0;
    if(!ops->file_close || !ops->file_read)return -EINVAL;
    void *state=NULL;int rc=ops->file_open(path->mount->context,&path->dentry->node,flags,&state);
    if(rc || !state) {
        if(state)ops->file_close(path->mount->context,&path->dentry->node,state);
        return rc;
    }
    struct vfs_open_file *file=kernel_malloc(sizeof(*file));
    if(!file){ops->file_close(path->mount->context,&path->dentry->node,state);return -ENOMEM;}
    *file=(struct vfs_open_file){.state=state};vfs_path_get(path,&file->path);*out=file;return 0;
}

/** @brief Read positional bytes through one retained per-open provider state.
 * @param file Borrowed live handle returned by vfs_file_open, required.
 * @param offset Linux byte position; handle ownership is unchanged.
 * @param buffer Kernel-accessible output, nullable only for zero capacity.
 * @param capacity Available output bytes.
 * @param read Required actual byte output, zero on validation/protocol error.
 * @return Provider status or negative EINVAL/EIO. The handle pins code and
 * inode even if an external identity (for example PID) has since retired.
 * Execution ownership required; no graph/module lock spans the callback.
 */
int vfs_file_read(struct vfs_open_file *file,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{
    if(!read)return -EINVAL;*read=0;
    if(!file || (!buffer && capacity))return -EINVAL;
    const struct vfs_path *path=&file->path;char empty;
    int rc=path->mount->ops->file_read(path->mount->context,&path->dentry->node,file->state,
        offset,buffer ? buffer : &empty,capacity,read,vfs_path_mount_flags(path));
    if(*read>capacity){*read=0;return -EIO;}
    return rc;
}

/** @brief Retire private open state before releasing its provider path/code.
 * @param file Owned optional handle; NULL does nothing. Caller clears its
 * stored pointer before entry so nested cleanup cannot release it twice.
 * @return None. Runs once per canonical OFD, after in-flight reads complete.
 * Execution ownership required; callbacks may perform I/O without any graph,
 * scheduler or module-table spinlock held by this function.
 */
void vfs_file_close(struct vfs_open_file *file)
{
    if(!file)return;
    struct vfs_path *path=&file->path;
    path->mount->ops->file_close(path->mount->context,&path->dentry->node,file->state);
    vfs_path_put(path);kernel_free(file);
}

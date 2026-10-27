/* SPDX-License-Identifier: Apache-2.0 */
#include "kernel_path_internal.h"
#include <reliefnt/mm.h>
#include <linux/fcntl.h>

struct kernel_read_state {struct vfs_path path;struct vfs_open_file *file;};

/** @brief Resolve and capture one trusted absolute file, retaining its code.
 * @param ns Borrowed initialized namespace under execution ownership.
 * @param name Required absolute kernel pathname.
 * @param out Required empty owned state, released by the caller on all exits.
 * @return Zero or negative lookup/type/capture errno. No graph/module lock
 * spans callbacks; the caller has already suspended asynchronous storage.
 */
static int kernel_read_open(struct vfs_namespace *ns,const char *name,struct kernel_read_state *out)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,VFS_LOOKUP_FOLLOW,&found);
    if(!rc)out->path=found.path;
    if(!rc && vfs_path_node(&out->path)->type!=RELIEFOS_FS_TYPE_FILE)
        rc=vfs_path_node(&out->path)->type==RELIEFOS_FS_TYPE_DIR ? -EISDIR : -EINVAL;
    if(!rc)rc=vfs_file_open(&out->path,LINUX_O_RDONLY,&out->file);
    return rc;
}

/** @brief Complete bounded positional reads through a single captured object.
 * @param state Borrowed actual path/open capture, required.
 * @param offset Initial byte offset. @param buffer Required writable kernel bytes.
 * @param capacity Number of bytes to fill unless EOF arrives first.
 * @param done Required count output, initially zero; error may leave a prefix.
 * @return Zero or negative provider/overflow errno; zero-byte success is EOF.
 * Execution ownership and synchronous storage required; pins outlive callbacks.
 */
static int kernel_read_bytes(struct kernel_read_state *state,uint64_t offset,
                                void *buffer,size_t capacity,size_t *done)
{
    while(*done<capacity) {
        if(*done>UINT64_MAX-offset)return -EOVERFLOW;
        size_t remaining=capacity-*done;
        uint32_t take=remaining>INT32_MAX ? INT32_MAX : (uint32_t)remaining,got=0;
        int rc=state->file ? vfs_file_read(state->file,offset+*done,(char *)buffer+*done,take,&got) :
            vfs_path_read(&state->path,offset+*done,(char *)buffer+*done,take,&got);
        if(rc)return rc;
        *done+=got;if(!got)break;
    }
    return 0;
}

/** @brief Retire a private capture before the surrounding retained inode path.
 * @param state Owned read state, required; leaves both handles empty.
 * @return None. Synchronous execution ownership required, no callback locks.
 */
static void kernel_read_close(struct kernel_read_state *state)
{
    struct vfs_open_file *file=state->file;state->file=NULL;vfs_file_close(file);
    vfs_path_put(&state->path);
}

/** @brief Read trusted positional bytes with balanced namespace/open ownership.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @param offset Initial byte position. @param buffer Destination, NULL at size0.
 * @param capacity Destination capacity. @param read Required actual count output.
 * @return Zero including EOF-short success or negative errno with count0.
 * Execution ownership required, no graph/module lock across callbacks. Saves
 * and restores async state, preserving old/new mutation markers on failure.
 */
int vfs_kernel_read(struct vfs_namespace *ns,const char *name,uint64_t offset,
                    void *buffer,uint32_t capacity,uint32_t *read)
{
    if(!read)return -EINVAL;*read=0;
    if(!buffer && capacity)return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();
    struct kernel_read_state state={0};size_t done=0;int rc=kernel_read_open(ns,name,&state);
    if(!rc)rc=kernel_read_bytes(&state,offset,buffer,capacity,&done);
    kernel_read_close(&state);
    vfs_kernel_io_end(scope);
    if(!rc)*read=(uint32_t)done;return rc;
}

/** @brief Read a trusted file's observed size, holding its provider throughout.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @param data Required owned page output, empty on failure.
 * @param length Required observed byte output, zero on failure.
 * @return Zero or negative errno. EOF before observed size is EIO; allocation
 * limits are checked without truncating to32 bits. Execution ownership required;
 * restores async state without clearing mutation latches and retires every
 * temporary path/capture on all exits.
 */
int vfs_kernel_read_file(struct vfs_namespace *ns,const char *name,const void **data,size_t *length)
{
    if(data)*data=NULL;if(length)*length=0;
    if(!data || !length)return -EINVAL;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();
    struct kernel_read_state state={0};struct linux_stat_abi status;void *buffer=NULL;
    uint64_t physical=0;uint32_t pages=0;size_t size=0,done=0;
    int rc=kernel_read_open(ns,name,&state);
    if(!rc)rc=vfs_path_stat(&state.path,&status);
    if(!rc) {
        if(status.st_size<0 || (uint64_t)status.st_size>SIZE_MAX-4095u)rc=-EFBIG;
        else {size=(size_t)status.st_size;uint64_t count=(size+4095u)/4096u;
            if(count>UINT32_MAX)rc=-EFBIG;else pages=count ? (uint32_t)count : 1;}
    }
    if(!rc) {
        physical=mm_alloc_pages(pages);
        if(!physical || !(buffer=mm_phys_to_virt(physical)))rc=-ENOMEM;
    }
    if(!rc)rc=kernel_read_bytes(&state,0,buffer,size,&done);
    if(!rc && done!=size)rc=-EIO;
    kernel_read_close(&state);
    if(rc && physical)mm_free_pages(physical,pages);
    vfs_kernel_io_end(scope);
    if(!rc){*data=buffer;*length=size;}return rc;
}

/** @brief Consume an owned kernel-file page buffer including empty files.
 * @param data Owned optional buffer. @param length Exact successful read size.
 * @return None. No provider callbacks; caller must not reuse the buffer.
 */
void vfs_kernel_file_free(const void *data,size_t length)
{
    if(!data)return;
    uint32_t pages=(uint32_t)((length+4095u)/4096u);
    mm_free_pages(mm_virt_to_phys(data),pages ? pages : 1);
}

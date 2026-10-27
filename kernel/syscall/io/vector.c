/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include "vector_internal.h"

/** @brief Canonicalize only ABI arguments consumed by a vector syscall.
 * @param number Native operation selector.
 * @param raw Borrowed six-register argument array.
 * @param out Required six-word fingerprint, no ownership change.
 * @return None. Pure, nonallocating execution-owned helper; high unused bits
 * and unused readv/writev position registers never alter syscall identity.
 */
static void regular_vector_args(uint64_t number,const uint64_t raw[6],uint64_t out[6])
{
    bool positional=number==LINUX_SYS_PREADV || number==LINUX_SYS_PWRITEV ||
        number==LINUX_SYS_PREADV2 || number==LINUX_SYS_PWRITEV2;
    *out=(uint32_t)raw[0];out[1]=raw[1];out[2]=raw[2];
    out[3]=positional ? raw[3] : 0;out[4]=0;
    out[5]=number==LINUX_SYS_PREADV2 || number==LINUX_SYS_PWRITEV2 ? (uint32_t)raw[5] : 0;
}

/** @brief Retire a pending vector before an unrelated syscall can reuse its FD.
 * @param task Borrowed execution-owned caller, nullable.
 * @param frame Borrowed native entry frame, nullable; sigreturn preserves state.
 * @return None. A mismatch invokes common queue/drain/provider cleanup before
 * new descriptor admission. Matching retries retain imported user metadata.
 */
void task_vector_enter(struct task *task,const struct trap_frame *frame)
{
    if(!task || !frame || !task->vector_io || frame->rax==LINUX_SYS_RT_SIGRETURN)return;
    uint64_t raw[]={frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8,frame->r9},args[6];
    regular_vector_args(frame->rax,raw,args);
    if(task->vector_io->number!=frame->rax || __builtin_memcmp(task->vector_io->args,args,sizeof(args)))
        task_release_syscall_file(task);
}

/** @brief Consume imported vector metadata after the caller drains owned I/O.
 * @param task Execution-owned caller, nullable; stored pointer clears first.
 * @return None. Caller has removed queued waits and drained storage before
 * entry. No OFD reference is owned here; common admission retains its provider.
 * No allocation, I/O or graph/module lock occurs during this release.
 */
void task_vector_cancel(struct task *task)
{
    if(!task)return;struct task_vector_state *state=task->vector_io;task->vector_io=NULL;kernel_free(state);
}

/** @brief Count accepted vector bytes before signal/common cancellation.
 * @param task Borrowed execution-owned owner, nullable.
 * @return Completed elements plus accepted current-element prefix, or zero
 * without a vector. Pure snapshot; no ownership change, allocation or I/O.
 */
uint64_t task_vector_progress(const struct task *task)
{return task && task->vector_io ? task->vector_io->completed+(task->regular_io.active ? task->regular_io.done : 0) : 0;}

/** @brief Finish one ordinary vector after all queued/kernel I/O is quiescent.
 * @param task Execution-owned caller with an owned imported vector.
 * @param file Borrowed canonical OFD still pinned by common admission.
 * @param error Final negative errno, or zero for EOF/short/full success.
 * @return Accepted prefix if nonzero, otherwise error. Removes waits and
 * drains storage before freeing metadata; actual file position already tracks
 * accepted nonpositional bytes. No provider/graph/module lock spans draining.
 */
static int64_t regular_vector_finish(struct task *task,struct task_file *file,int error)
{
    uint64_t accepted=task_vector_progress(task);
    if(task->waiting_queue)kernel_wait_queue_remove(task->waiting_queue,task);
    storage_drain_task_io(task->pid);task->regular_io.active=false;
    task_vector_cancel(task);if(file->io_owner==task->pid)file->io_owner=0;
    return accepted ? (int64_t)accepted : error;
}

/** @brief Import once and resume ordinary-file vector data through actual OFDs.
 * @param task Execution-owned caller retaining the admitted file description.
 * @param file Borrowed actual provider file, pinned until common cleanup.
 * @param number One of the six Linux vectored operation selectors.
 * @param raw Borrowed six-register syscall operands, never retained as pointers.
 * @param writing Write rather than read validated user buffers.
 * @param positional Preserve shared file offset; v2 offset=-1 is false here.
 * @param position Captured explicit byte position for positional operations.
 * @return Total bytes, negative errno or internal retry preserving imported
 * iovecs and accepted progress. The OFD position stays reserved across waits;
 * positional operations never temporarily change it. User payload is accessed
 * normally; only iovec metadata is snapshotted. No graph/module lock spans I/O.
 */
static int64_t regular_vector_io(struct task *task,struct task_file *file,uint64_t number,
                                   const uint64_t raw[6],bool writing,bool positional,uint64_t position)
{
    if(!task || task->syscall_file!=file || task->syscall_file_number!=number)return -RELIEFOS_EBADF;
    struct task_vector_state *state=task->vector_io;
    if(!state) {
        uint32_t count=(uint32_t)raw[2];
        if(!user_range_ok(raw[1],count*sizeof(struct iovec)))return -RELIEFOS_EFAULT;
        state=kernel_malloc(sizeof(*state)+count*sizeof(struct iovec));if(!state)return -RELIEFOS_ENOMEM;
        *state=(struct task_vector_state){.number=number,.position=positional ? position : 0,
            .count=count,.writing=writing,.positional=positional};regular_vector_args(number,raw,state->args);
        __builtin_memcpy(state->vectors,(const void *)(uintptr_t)raw[1],count*sizeof(struct iovec));
        for(uint32_t i=0;i<count;++i) {
            struct iovec *vector=&state->vectors[i];int error=0;
            if((int64_t)vector->iov_len<0)error=-RELIEFOS_EINVAL;
            else {
                if(vector->iov_len>0x7ffff000u-state->requested)vector->iov_len=0x7ffff000u-state->requested;
                if(vector->iov_len && !(writing ? user_range_ok((uintptr_t)vector->iov_base,vector->iov_len) :
                    user_range_writable((uintptr_t)vector->iov_base,vector->iov_len)))error=-RELIEFOS_EFAULT;
            }
            if(error){kernel_free(state);return error;}state->requested+=vector->iov_len;
        }
        if(!state->requested){kernel_free(state);return 0;}
        task->vector_io=state;
    }
    if(file->io_owner && file->io_owner!=task->pid)return -RELIEFOS_EAGAIN;
    file->io_owner=task->pid;
    while(state->index<state->count) {
        const struct iovec *vector=&state->vectors[state->index];
        if(!vector->iov_len){++state->index;continue;}
        uint64_t done=task->regular_io.active ? task->regular_io.done : 0;
        if(done>vector->iov_len)return regular_vector_finish(task,file,-LINUX_EIO);
        uint64_t address=(uintptr_t)vector->iov_base+done,length=vector->iov_len-done;
        if(length && !(state->writing ? user_range_ok(address,length) : user_range_writable(address,length)))
            return regular_vector_finish(task,file,-RELIEFOS_EFAULT);
        file->io_owner=task->pid;
        int64_t part=syscall_regular_io(task,file,(uintptr_t)vector->iov_base,vector->iov_len,
            state->position+state->completed,state->writing,state->positional);
        if(part==-RELIEFOS_EAGAIN || part==KERNEL_SYSCALL_BLOCKED)return part;
        if(part<0)return regular_vector_finish(task,file,(int)part);
        if((uint64_t)part>vector->iov_len)return regular_vector_finish(task,file,-LINUX_EIO);
        state->completed+=(uint64_t)part;
        if((uint64_t)part<vector->iov_len)break;
        ++state->index;
    }
    return regular_vector_finish(task,file,0);
}

/** @brief Dispatch a Linux vector request through its admitted description.
 * @param task Borrowed execution-owned caller retaining the admitted OFD.
 * @param number READV/WRITEV/PREADV/PWRITEV/PREADV2/PWRITEV2 operation number.
 * @param a0 Native descriptor argument, signed low32bits.
 * @param a1 User iovec array address.
 * @param a2 Native vector count.
 * @param a3 Full64bit explicit position, ignored for ordinary readv/writev.
 * @param a4 Unused native x86_64 pos_h register; no consumed bits.
 * @param a5 Low32bit v2 RWF flags, ignored for older variants.
 * @return Accepted bytes, negative errno or internal retry. Ordinary FS files
 * own imported iovec metadata and accepted progress until completion/common
 * cancellation. Specialized device/socket APIs preserve their own handling.
 * All backend work runs under execution ownership without graph/module locks.
 */
int64_t syscall_vector_io(struct task *task,uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,
                            uint64_t a3,uint64_t a4,uint64_t a5)
{
    struct task_file *file = task_file_for_io(task, (int)a0);
    bool writing = number == LINUX_SYS_WRITEV || number == LINUX_SYS_PWRITEV ||
                   number == LINUX_SYS_PWRITEV2;
    bool positional = number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
                      number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2;
    uint64_t position = a3;
    bool current_position = (number == LINUX_SYS_PREADV2 ||
                            number == LINUX_SYS_PWRITEV2) &&
                           position == UINT64_MAX;
    bool provider_readv=file && file->character_file && !writing &&
        character_file_has_readv(file->character_file);
    uint64_t positional_flags = (number == LINUX_SYS_PREADV2 ||
                                 number == LINUX_SYS_PWRITEV2) ? (uint32_t)a5 : 0;
    uint64_t saved_offset = 0;
    bool restore_position = false;
    int64_t total;
    if (file && file->kind == TASK_FILE_KIND_SIGNALFD) {
        if (positional && !current_position) return (int64_t)position < 0 ? -LINUX_EINVAL : -LINUX_ESPIPE;
        if (writing) return -LINUX_EINVAL;
        return task_signalfd_readv(task, file, a1, a2, (uint32_t)positional_flags);
    }
    if (positional && !current_position && (int64_t)position < 0)
        return -RELIEFOS_EINVAL;
    if (positional_flags && !provider_readv)
        return -RELIEFOS_EOPNOTSUPP;
    if (file ? !(writing ? file_can_write(file) : file_can_read(file)) :
        (!task_pty_for_io(task, (int)a0) &&
         ((uint32_t)a0 >= SCHED_TASK_STDIO_MAX ||
          (sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)a0))))) return -RELIEFOS_EBADF;
    if (a2 > UIO_MAXIOV) return -RELIEFOS_EINVAL;
    if (!a2) return provider_readv ? character_file_readv(file->character_file,task,file,NULL,0,
        positional && !current_position,position,positional_flags) : 0;
    if(file && file->node.type==RELIEFOS_FS_TYPE_FILE && file->path_ref.mount &&
       !file->character_file &&
       !(file->flags&(TASK_FILE_FLAG_DEV_NODE|TASK_FILE_FLAG_DEV_SHM|TASK_FILE_FLAG_SOCKET_UNIX|TASK_FILE_FLAG_SOCKET_INET))) {
        const uint64_t raw[]={a0,a1,a2,a3,a4,a5};
        return regular_vector_io(task,file,number,raw,writing,positional && !current_position,position);
    }
    if (!user_range_ok(a1, a2 * sizeof(struct iovec))) return -RELIEFOS_EFAULT;
    struct iovec iov[UIO_MAXIOV];
    const struct iovec *user_iov = (const struct iovec *)(uintptr_t)a1;
    uint64_t requested = 0;
    for (uint64_t i = 0; i < a2; ++i) {
        iov[i] = user_iov[i];
        if ((int64_t)iov[i].iov_len < 0) return -RELIEFOS_EINVAL;
        if (iov[i].iov_len > 0x7ffff000u - requested) iov[i].iov_len = 0x7ffff000u - requested;
        if (iov[i].iov_len && !(writing ? user_range_ok((uintptr_t)iov[i].iov_base, iov[i].iov_len) :
            user_range_writable((uintptr_t)iov[i].iov_base, iov[i].iov_len))) return -RELIEFOS_EFAULT;
        requested += iov[i].iov_len;
    }
    if(provider_readv)return character_file_readv(file->character_file,task,file,iov,a2,
        positional && !current_position,position,positional_flags);
    if (!requested) return 0;
    if (positional && !current_position) {
        if (!file) return -RELIEFOS_EINVAL;
        if (file->node.type != RELIEFOS_FS_TYPE_FILE) return -RELIEFOS_ESPIPE;
        saved_offset = file->offset;
        file->offset = position;
        restore_position = true;
    }
    if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
        if (restore_position) file->offset = saved_offset;
        return task_socket_vector(file, iov, (uint32_t)a2, requested, writing);
    }
    total = 0;
    for (uint64_t i = 0; i < a2; ++i) {
        if (!iov[i].iov_len) continue;
        /* The vector loop already validated its request. Reuse the same
         * scalar checks without recursively allocating the large legacy
         * dispatcher frame for every iovec. */
        int64_t part = writing
            ? syscall_io_write(a0, (uintptr_t)iov[i].iov_base, iov[i].iov_len)
            : syscall_io_read(a0, (uintptr_t)iov[i].iov_base, iov[i].iov_len);
        if (part < 0) {
            if (restore_position) file->offset = saved_offset;
            return total ? total : part;
        }
        total += part;
        if ((uint64_t)part < iov[i].iov_len) break;
    }
    if (restore_position) file->offset = saved_offset;
    return total;
}

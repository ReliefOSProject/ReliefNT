/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/* One original invocation owns both OFDs and any bytes read before a queue
 * wait. Filesystem callbacks are synchronous for this kernel staging buffer;
 * stream/provider EAGAIN still preserves the buffer and descriptor identity. */
struct task_transfer_state {
    struct task_file *input,*output;
    uint64_t args[6],number,input_user,output_user;
    uint64_t input_position,output_position,count,total;
    uint32_t buffered,consumed;
    bool range,locked,prepared,waiting_io,done;
    int error;
    uint8_t bytes[RELIEFOS_FS_IO_SLICE_BYTES];
};

/** @brief Release a suspended copy's kernel buffer and original descriptors.
 * @param task Execution-owned caller, nullable.
 * Detaches state and unlinks any wait record before draining I/O and final
 * OFD puts. Offset reservations are
 * released only when owned by this task; both references retire independently
 * even when they name the same OFD. No scheduler/graph/module lock spans I/O.
 */
void task_transfer_cancel(struct task *task)
{
    if(!task || !task->transfer_io)return;
    struct task_transfer_state *state=task->transfer_io;task->transfer_io=NULL;
    if(task->waiting_queue)kernel_wait_queue_remove(task->waiting_queue,task);
    if(state->waiting_io)storage_drain_task_io(task->pid);
    if(state->locked) {
        if(state->input->io_owner==task->pid)state->input->io_owner=0;
        if(state->output->io_owner==task->pid)state->output->io_owner=0;
    }
    task_file_put(state->input);task_file_put(state->output);kernel_free(state);
}

/** @brief Retire a different invocation before descriptor admission can reuse it.
 * @param task Current execution-owned task, nullable.
 * @param frame Borrowed syscall registers, nullable.
 * Matching meaningful argument registers keep original OFDs and copied offset
 * inputs. No allocation; common retirement may drain old I/O without locks.
 */
void task_transfer_enter(struct task *task,const struct trap_frame *frame)
{
    if(!task || !frame || !task->transfer_io || frame->rax==LINUX_SYS_RT_SIGRETURN)return;
    const struct task_transfer_state *state=task->transfer_io;
    uint64_t args[]={frame->rdi,frame->rsi,frame->rdx,frame->r10,
        state->range ? frame->r8 : 0,state->range ? frame->r9 : 0};
    if(state->number!=frame->rax || __builtin_memcmp(state->args,args,sizeof(args)))
        task_release_syscall_file(task);
}

/** @brief Capture an explicit offset once before the first retryable operation.
 * @param address User signed-off_t input/output pointer, or zero for OFD offset.
 * @param position Receives the checked nonnegative value for nonzero address.
 * @return Zero or negative EFAULT/EINVAL. Execution-owned usercopy may fault;
 * no filesystem/module/graph lock is held and no pointer is retained as data.
 */
static int transfer_offset(uint64_t address,uint64_t *position)
{
    if(!address)return 0;
    if(!user_range_ok(address,sizeof(int64_t)) || !user_range_writable(address,sizeof(int64_t)))return -EFAULT;
    int64_t value=*(const int64_t *)(uintptr_t)address;
    if(value<0)return -EINVAL;
    *position=(uint64_t)value;return 0;
}

/** @brief Own both canonical descriptors and immutable syscall offset inputs.
 * @param task Current execution-owned task, required.
 * @param range Select copy_file_range rather than sendfile.
 * @param args Six kernel-owned native argument values, required.
 * @return Zero or negative fd/access/type/allocation/input errno. Failure
 * retires every acquired pin; no provider callback runs before both OFDs are
 * retained. Matching retries use original objects after close/fd reuse.
 */
static int transfer_begin(struct task *task,bool range,const uint64_t args[6])
{
    uint64_t number=range ? LINUX_SYS_COPY_FILE_RANGE : LINUX_SYS_SENDFILE;
    struct task_transfer_state *state=task->transfer_io;
    if(state) {
        if(state->number==number && !__builtin_memcmp(state->args,args,sizeof(state->args)))return 0;
        task_transfer_cancel(task);
    }
    if(range && (uint32_t)args[5])return -EINVAL;
    int32_t input_fd=(int32_t)args[range ? 0 : 1],output_fd=(int32_t)args[range ? 2 : 0];
    struct task_file *input=task_file_for_io(task,input_fd),*output=task_file_for_io(task,output_fd);
    if(!input || !output || (input->flags&TASK_FILE_FLAG_PATH) || (output->flags&TASK_FILE_FLAG_PATH))return -EBADF;
    if(range && (input->node.type==RELIEFOS_FS_TYPE_DIR || output->node.type==RELIEFOS_FS_TYPE_DIR))return -EISDIR;
    if(!file_can_read(input) || !file_can_write(output))return -EBADF;
    if(input->node.type!=RELIEFOS_FS_TYPE_FILE || (input->node.flags&STORAGE_NODE_FLAG_PROC))return -EINVAL;
    if(range && output->node.type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    if(!range && output->node.type!=RELIEFOS_FS_TYPE_FILE &&
       !(output->flags&(TASK_FILE_FLAG_PIPE|TASK_FILE_FLAG_SOCKET_UNIX|TASK_FILE_FLAG_SOCKET_INET)))return -EINVAL;
    if(output->flags&RELIEFOS_O_APPEND)return range ? -EBADF : -EINVAL;
    if(!vfs_path_node(&input->path_ref) ||
       (output->node.type==RELIEFOS_FS_TYPE_FILE && !vfs_path_node(&output->path_ref)))return -EBADF;
    if(range && !vfs_path_same_filesystem(&input->path_ref,&output->path_ref))return -EXDEV;
    state=kernel_malloc(sizeof(*state));if(!state)return -ENOMEM;
    *state=(struct task_transfer_state){.range=range,.number=number,
        .input_user=args[range ? 1 : 2],.output_user=range ? args[3] : 0};
    __builtin_memcpy(state->args,args,sizeof(state->args));
    uint64_t count=args[range ? 4 : 3];state->count=count>0x7ffff000ULL ? 0x7ffff000ULL : count;
    task->transfer_io=state;
    state->input=task_file_get(input);
    if(state->input)state->output=task_file_get(output);
    int rc=(!state->input || !state->output) ? -ENOMEM : transfer_offset(state->input_user,&state->input_position);
    if(!rc)rc=transfer_offset(state->output_user,&state->output_position);
    if(rc)task_transfer_cancel(task);
    return rc;
}

/** @brief Reserve shared offsets together, then validate the actual source range.
 * @param task Execution-owned caller whose transfer state owns both OFDs.
 * @return Zero or negative busy/metadata/range errno. EAGAIN retains original
 * offsets and provider pins. No partial new reservation is acquired when the
 * peer OFD is owned by another task. Provider metadata I/O is synchronous.
 */
static int transfer_prepare(struct task *task)
{
    struct task_transfer_state *state=task->transfer_io;
    if(state->prepared)return 0;
    if(!state->locked) {
        if((state->input->io_owner && state->input->io_owner!=task->pid) ||
           (state->output->io_owner && state->output->io_owner!=task->pid))return -EAGAIN;
        state->input->io_owner=state->output->io_owner=task->pid;state->locked=true;
        if(!state->input_user)state->input_position=state->input->offset;
        if(!state->output_user)state->output_position=state->output->offset;
    }
    if(state->input_position>INT64_MAX || state->output_position>INT64_MAX)return -EOVERFLOW;
    if(state->count>(uint64_t)INT64_MAX-state->input_position ||
       (state->output->node.type==RELIEFOS_FS_TYPE_FILE && state->count>(uint64_t)INT64_MAX-state->output_position))return -EOVERFLOW;
    struct linux_stat_abi status;
    int rc=vfs_path_stat(&state->input->path_ref,&status);
    if(rc){state->waiting_io=rc==-EAGAIN;return rc;}
    state->waiting_io=false;
    if(status.st_size<0)return -EIO;
    uint64_t size=(uint64_t)status.st_size;
    if(state->input_position>=size)state->count=0;
    else if(state->count>size-state->input_position)state->count=size-state->input_position;
    if(state->range && state->count && vfs_path_same_inode(&state->input->path_ref,&state->output->path_ref) &&
       state->output_position<state->input_position+state->count &&
       state->input_position<state->output_position+state->count)return -EINVAL;
    state->prepared=true;return 0;
}

/** @brief Publish accepted offsets without rereading mutable user inputs.
 * @param task Pinned destination task, whose MM may differ from active CR3.
 * @param state Borrowed transfer whose original offset snapshots remain live.
 * @return Zero or EFAULT if a destination is no longer writable. Execution
 * ownership is required; usercopy may fault without graph/module spinlocks.
 * This helper neither releases state nor acquires new filesystem references.
 */
static int transfer_publish_offsets(struct task *task,const struct task_transfer_state *state)
{
    const uint64_t pointers[]={state->input_user,state->output_user};
    const uint64_t positions[]={state->input_position,state->output_position};
    for(unsigned i=0;i<2;++i)if(pointers[i]) {
        int64_t value=(int64_t)positions[i];
        if(user_copy_to_task(task,pointers[i],&value,sizeof(value))<0)return -EFAULT;
    }
    return 0;
}

/** @brief Keep accepted copy bytes visible to an interrupted syscall's caller.
 * @param task Current execution-owned task, nullable.
 * @return Accepted bytes, zero before any output, or EFAULT for an offset
 * output mapping removed during the wait. Drains before publication but leaves
 * original OFDs/state for common cleanup; no graph/module lock spans either.
 */
int64_t task_transfer_interrupt(struct task *task)
{
    if(!task || !task->transfer_io)return 0;
    struct task_transfer_state *state=task->transfer_io;
    if(state->waiting_io){storage_drain_task_io(task->pid);state->waiting_io=false;}
    if(!state->total)return 0;
    int rc=transfer_publish_offsets(task,state);return rc ? rc : (int64_t)state->total;
}

/** @brief Publish final offset outputs and release a completed copy exactly once.
 * @param task Current execution-owned task retaining its transfer state.
 * @return Completed bytes or negative errno; synchronous-file sync EAGAIN
 * retains state for its flush only. An immediately committed proc diagnostic
 * has no deferred flush. User output addresses are revalidated before writing
 * and never reread for new input. Final puts run without locks.
 */
static int64_t transfer_complete(struct task *task)
{
    struct task_transfer_state *state=task->transfer_io;
    int64_t result=state->total ? (int64_t)state->total : state->error;
    if(state->total && state->output->node.type==RELIEFOS_FS_TYPE_FILE && (state->output->flags&LINUX_O_DSYNC)) {
        int rc=vfs_path_sync_after_write(&state->output->path_ref,(state->output->flags&LINUX_O_SYNC)==LINUX_O_DSYNC);
        if(rc==-EAGAIN){state->waiting_io=true;return rc;}
        if(rc)result=rc;
    }
    if(result>=0){int rc=transfer_publish_offsets(task,state);if(rc)result=rc;}
    task_transfer_cancel(task);return result;
}

/** @brief Copy retained source bytes to the original output description.
 * @param state Owned transfer metadata with unread buffered bytes, required.
 * @param written Required output count, cleared before dispatch.
 * @return Zero or provider/stream errno, preserving short accepted progress.
 * Callbacks run under execution ownership without graph/module locks; the
 * state owns provider code and stable kernel staging bytes through waits.
 */
static int transfer_output(struct task_transfer_state *state,uint32_t *written)
{
    struct task_file *output=state->output;
    const void *bytes=state->bytes+state->consumed;
    uint32_t count=state->buffered-state->consumed;*written=0;
    if(output->node.type==RELIEFOS_FS_TYPE_FILE)
        return vfs_path_write(&output->path_ref,state->output_position,bytes,count,written);
    int rc=output->flags&TASK_FILE_FLAG_PIPE ? task_pipe_write(output,bytes,count) :
        output->flags&TASK_FILE_FLAG_SOCKET_UNIX ? task_socket_write(output,bytes,count) :
        task_inet_write(output,bytes,count);
    if(rc<0)return rc;
    if((uint32_t)rc>count)return -EIO;
    *written=(uint32_t)rc;return 0;
}

/** @brief Drive a read/write phase pair without replaying already consumed data.
 * @param task Current execution-owned caller owning both descriptors.
 * @return Bytes, errno, or an internal EAGAIN/BLOCKED retry. Blocking waits
 * retain buffered data and offset snapshots even after partial output;
 * nonblocking stream EAGAIN returns accepted progress when nonzero.
 * Filesystem I/O is synchronous
 * for kernel-buffer safety; after a completed chunk, a 2ms fairness boundary
 * returns legal positive progress rather than imposing a fixed copy ceiling.
 */
static int64_t transfer_run(struct task *task)
{
    struct task_transfer_state *state=task->transfer_io;
    storage_set_io_async_context(false);
    if(state->done)return transfer_complete(task);
    int rc=transfer_prepare(task);
    if(rc==-EAGAIN)return rc;
    if(rc){state->error=rc;state->done=true;return transfer_complete(task);}
    uint64_t started=time_uptime_us();
    while(state->total<state->count) {
        if(!state->buffered) {
            uint64_t remaining=state->count-state->total;
            uint32_t length=remaining>sizeof(state->bytes) ? (uint32_t)sizeof(state->bytes) : (uint32_t)remaining;
            uint32_t read=0;
            rc=vfs_path_read_cursor(&state->input->path_ref,state->input_position,state->bytes,length,&read,&state->input->read_cursor);
            if(rc==-EAGAIN) {
                state->waiting_io=true;
                return rc;
            }
            state->waiting_io=false;
            if(rc){state->error=rc;break;}
            if(!read)break;
            state->buffered=read;state->consumed=0;
        }
        uint32_t written=0;
        rc=transfer_output(state,&written);
        if(written) {
            state->consumed+=written;state->total+=written;state->input_position+=written;
            if(!state->input_user)state->input->offset=state->input_position;
            if(state->output->node.type==RELIEFOS_FS_TYPE_FILE) {
                state->output_position+=written;
                if(!state->output_user)state->output->offset=state->output_position;
                state->output->node=*vfs_path_node(&state->output->path_ref);state->output->read_cursor.valid=0;
            }
        }
        if(rc==KERNEL_SYSCALL_BLOCKED)return rc;
        if(rc==-EAGAIN) {
            state->waiting_io=state->output->node.type==RELIEFOS_FS_TYPE_FILE;
            bool nonblocking_stream=!state->waiting_io &&
                (state->output->flags&RELIEFOS_O_NONBLOCK);
            if(!nonblocking_stream || !state->total)return rc;
            break;
        }
        state->waiting_io=false;
        if(rc){state->error=rc;break;}
        if(!written)break;
        if(state->consumed<state->buffered)continue;
        state->buffered=state->consumed=0;
        if(time_uptime_us()-started>=2000)break;
    }
    state->done=true;return transfer_complete(task);
}

/** @brief Transfer a regular input to a retained regular file or stream.
 * @param out_fd_arg Signed low-32-bit writable descriptor number.
 * @param in_fd_arg Signed low-32-bit readable regular descriptor number.
 * @param offset_arg Optional user signed-off_t input/output pointer.
 * @param count_arg Requested native byte count, capped to Linux MAX_RW_COUNT.
 * @return Bytes, negative errno or internal EAGAIN retry. Owns both canonical
 * OFDs and kernel bytes across waits; never resolves a diagnostic name. Caller
 * owns execution without graph/module locks. Identical OFDs are permitted.
 */
int64_t syscall_sendfile(uint64_t out_fd_arg,uint64_t in_fd_arg,uint64_t offset_arg,uint64_t count_arg)
{
    struct task *task=sched_current_task();if(!task)return -EPERM;
    uint64_t args[]={out_fd_arg,in_fd_arg,offset_arg,count_arg,0,0};
    int rc=transfer_begin(task,false,args);return rc ? rc : transfer_run(task);
}

/** @brief Copy a nonoverlapping range inside one retained filesystem instance.
 * @param in_fd_arg Signed low-32-bit readable regular descriptor number.
 * @param off_in_arg Optional user signed-off_t source input/output pointer.
 * @param out_fd_arg Signed low-32-bit writable regular descriptor number.
 * @param off_out_arg Optional user signed-off_t destination input/output pointer.
 * @param len_arg Native requested bytes, capped to Linux MAX_RW_COUNT.
 * @param flags_arg Low-32-bit flags, currently zero; upper bits are ignored.
 * @return Bytes, negative errno or internal EAGAIN retry. Same inode with
 * nonoverlapping ranges is supported; append returns EBADF, directory EISDIR,
 * unsupported cross-filesystem copy EXDEV. Owns original OFDs/offset inputs
 * and staging bytes through retries; no names or graph/module locks across I/O.
 */
int64_t syscall_copy_file_range(uint64_t in_fd_arg,uint64_t off_in_arg,uint64_t out_fd_arg,
                                   uint64_t off_out_arg,uint64_t len_arg,uint64_t flags_arg)
{
    struct task *task=sched_current_task();if(!task)return -EPERM;
    uint64_t args[]={in_fd_arg,off_in_arg,out_fd_arg,off_out_arg,len_arg,flags_arg};
    int rc=transfer_begin(task,true,args);return rc ? rc : transfer_run(task);
}

/* Linux 6.12 net/socket.c sendmmsg/recvmmsg iteration and restart state. */
#include <reliefnt/syscall_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/futex.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/time.h>
#include <reliefnt/heap.h>
#include <reliefnt/swap.h>
#include <linux/socket.h>
#include <linux/errno.h>

/** @brief Update recvmmsg's remaining timeout after a successfully copied message. */
static bool mmsg_update_timeout(struct task_mmsg_state *state)
{
    struct linux_timespec now;
    time_clock_get(LINUX_CLOCK_MONOTONIC, &now);
    int64_t seconds = state->deadline.tv_sec - now.tv_sec;
    int64_t ns = state->deadline.tv_nsec - now.tv_nsec;
    if (ns < 0) { --seconds; ns += 1000000000; }
    if (seconds < 0) seconds = ns = 0;
    state->remaining = (struct linux_timespec){seconds, ns};
    return seconds == 0 && ns == 0;
}

struct socket_batch_meta {bool finished;int error;};
struct socket_batch_receipt {bool completed;int64_t result;uint64_t requested;uint32_t flags;size_t cursor_after;};

#ifdef CONFIG_SWAP
/** @brief Reserve one scope-owned batch record before any message side effect.
 * @param task Execution-owned caller. @param kind Private slot discriminator.
 * @param bytes Record size. @param output Required pointer, NULL if unscoped.
 * @return Zero or negative allocation/state error. Common scope cleanup frees
 * the record; matching retries borrow it without repeating completed work.
 */
static int socket_batch_slot(struct task *task,uint32_t kind,size_t bytes,void **output)
{
    struct task_lookup_path *slot=NULL;int ret=task_lookup_scope_next(task,&slot);*output=NULL;
    if(ret || !slot)return ret;
    if(!slot->name) {
        slot->name=kernel_malloc(bytes);if(!slot->name)return -LINUX_ENOMEM;
        __builtin_memset(slot->name,0,bytes);slot->flags=kind;
    } else if(slot->flags!=kind)return -LINUX_EINVAL;
    *output=slot->name;return 0;
}

/** @brief Test whether a batch has an accepted result awaiting user output.
 * @param task Execution-owned task. @return True only for an owned receipt or
 * terminal count whose output has not completed. No cursor or I/O change.
 */
bool task_socket_batch_accepted(struct task *task)
{
    if(!task || !task->mmsg.active || !task->lookup_scope.armed)return false;
    struct task_lookup_path *slot=task_lookup_scope_at(task,0);
    if(slot && slot->flags==SOCKET_BATCH_META_KIND && slot->name &&
       ((struct socket_batch_meta *)slot->name)->finished && task->mmsg.count)return true;
    slot=task_lookup_scope_at(task,task->mmsg.lookup_cursor);
    return slot && slot->flags==SOCKET_BATCH_RECEIPT_KIND && slot->name &&
           ((struct socket_batch_receipt *)slot->name)->completed;
}

/** @brief Prepare batch interrupt outputs before removing a caught signal.
 * @param task Live execution-owned owner. @return Zero or transient contention.
 * Permanent invalid mappings follow the existing late-copy EFAULT behavior.
 */
int task_socket_batch_signal_prepare(struct task *task)
{
    if(!task || !task->mmsg.active)return 0;
    struct task_mmsg_state *state=&task->mmsg;
    if(state->receiving && task->socket_receive_done) {
        uint64_t offset=(uint64_t)state->count*sizeof(struct mmsghdr)+__builtin_offsetof(struct mmsghdr,msg_len);
        if(state->vector<=UINT64_MAX-offset) {
            int ret=swap_prepare_user_range(task,state->vector+offset,sizeof(uint32_t),true);
            if(ret==SWAP_STORAGE_CONTENDED)return ret;
        }
    }
    if(state->receiving && state->timeout_pointer && (state->count || task->socket_receive_done)) {
        int ret=swap_prepare_user_range(task,state->timeout_pointer,sizeof(state->remaining),true);
        if(ret==SWAP_STORAGE_CONTENDED)return ret;
    }
    return 0;
}
#endif

/** @brief Return completed messages, writing the timeout only for positive results. */
static int64_t mmsg_finish(struct task *task, struct task_file *file, int error,
                           struct socket_batch_meta *meta,bool prepare)
{
    struct task_mmsg_state state = task->mmsg;
#ifdef CONFIG_SWAP
    if(prepare && state.count && state.receiving && state.timeout_pointer) {
        int ret=swap_prepare_user_range(task,state.timeout_pointer,sizeof(state.remaining),true);
        if(ret==SWAP_STORAGE_CONTENDED) {
            if(meta){meta->finished=true;meta->error=error;}
            return KERNEL_SYSCALL_SWAP_CONTENDED;
        }
    }
#else
    (void)meta;(void)prepare;
#endif
    task->mmsg = (struct task_mmsg_state){0};
    if (!state.count) return error;
    if (state.receiving && error && error != -LINUX_EAGAIN)
        task_socket_message_error(file, -error, true);
    if (state.receiving && state.timeout_pointer &&
        user_copy_to_task(task, state.timeout_pointer, &state.remaining, sizeof(state.remaining)) < 0)
        return -LINUX_EFAULT;
    return state.count;
}

/**
 * @brief Run a Linux message batch, preserving the current slot during blocking.
 * @param receiving True for recvmmsg, false for sendmmsg.
 * @param fd Native signed descriptor, pinned by syscall_dispatch_frame.
 * @param vector Native 64-byte mmsghdr array in user memory.
 * @param length Unsigned message count; only sendmmsg caps it at UIO_MAXIOV.
 * @param flags Native unsigned MSG flags.
 * @param timeout recvmmsg timespec pointer; zero for no batch timeout.
 * @return Completed message count, negative errno, or KERNEL_SYSCALL_BLOCKED.
 * The dispatcher pins the original OFD and owns execution. Outgoing scoped
 * operands resume at the current message; no completed message is repeated.
 * No scheduler, filesystem graph or module-table lock spans socket callbacks.
 */
int64_t syscall_socket_mmsg(bool receiving, int fd, uint64_t vector,
                            uint32_t length, uint32_t flags, uint64_t timeout)
{
    struct task *task = sched_current_task();
    if (flags & MSG_CMSG_COMPAT) return -LINUX_EINVAL;
    if (!task) return -LINUX_ESRCH;
    struct task_file *file = task_file_for_io(task, fd);
    struct task_mmsg_state *state = &task->mmsg;
    struct socket_batch_meta local_meta={0},*meta=&local_meta;
#ifdef CONFIG_SWAP
    struct socket_batch_meta *saved_meta=NULL;
    int meta_status=socket_batch_slot(task,SOCKET_BATCH_META_KIND,sizeof(*saved_meta),(void **)&saved_meta);
    if(meta_status)return meta_status;
    if(saved_meta)meta=saved_meta;
    if(state->active && meta->finished)return mmsg_finish(task,file,meta->error,meta,true);
#endif
    if (!state->active) {
        struct task_mmsg_state initial = {.active = true, .receiving = receiving,
            .vector = vector, .length = !receiving && length > 1024 ? 1024 : length,
            .flags = flags, .timeout_pointer = receiving ? timeout : 0,
            .lookup_cursor = task->lookup_scope.cursor};
        if (initial.timeout_pointer) {
#ifdef CONFIG_SWAP
            struct task_user_text imported={0};
            int prep=task_lookup_user_bytes(task,timeout,sizeof(initial.remaining),&imported);
            if(prep)return prep;
            __builtin_memcpy(&initial.remaining,imported.text,sizeof(initial.remaining));kernel_free(imported.allocation);
#else
            if (!user_range_ok(timeout, sizeof(initial.remaining))) return -LINUX_EFAULT;
            __builtin_memcpy(&initial.remaining, (const void *)(uintptr_t)timeout, sizeof(initial.remaining));
#endif
            if (initial.remaining.tv_sec < 0 || (uint64_t)initial.remaining.tv_nsec >= 1000000000)
                return -LINUX_EINVAL;
            if (initial.remaining.tv_sec || initial.remaining.tv_nsec) {
                struct linux_timespec now;
                time_clock_get(LINUX_CLOCK_MONOTONIC, &now);
                int64_t carry = (now.tv_nsec + initial.remaining.tv_nsec) / 1000000000;
                if (initial.remaining.tv_sec > INT64_MAX - now.tv_sec - carry)
                    initial.deadline = (struct linux_timespec){INT64_MAX, 999999999};
                else initial.deadline = (struct linux_timespec){
                    now.tv_sec + initial.remaining.tv_sec + carry,
                    (now.tv_nsec + initial.remaining.tv_nsec) % 1000000000};
            }
        }
        if (!file) return -LINUX_EBADF;
        if (!(file->flags & TASK_FILE_FLAG_SOCKET)) return -LINUX_ENOTSOCK;
        if (receiving && !(flags & MSG_ERRQUEUE)) {
            int error = task_socket_message_error(file, 0, false);
            if (error) return -error;
        }
        initial.lookup_cursor=task->lookup_scope.cursor;
        *state = initial;
    }
    int error = 0;
    while (state->count < state->length) {
        uint64_t offset = (uint64_t)state->count * sizeof(struct mmsghdr);
        if (state->vector > UINT64_MAX - offset - sizeof(struct mmsghdr)) {
            error = -LINUX_EFAULT;
            break;
        }
        uint64_t message = state->vector + offset;
        uint32_t message_flags = state->flags;
        if (receiving) {
            message_flags &= ~MSG_WAITFORONE;
            if (state->count && (state->flags & MSG_WAITFORONE)) message_flags |= MSG_DONTWAIT;
        } else if (state->count + 1 < state->length) message_flags |= MSG_BATCH;
        struct socket_message_result output = {0};
        if(task->lookup_scope.armed)task->lookup_scope.cursor=state->lookup_cursor;
        struct socket_batch_receipt local_receipt={0},*receipt=&local_receipt;
#ifdef CONFIG_SWAP
        struct socket_batch_receipt *saved_receipt=NULL;
        int saved_status=socket_batch_slot(task,SOCKET_BATCH_RECEIPT_KIND,sizeof(*saved_receipt),(void **)&saved_receipt);
        if(saved_status){error=saved_status;break;}
        if(saved_receipt)receipt=saved_receipt;
#endif
        int64_t result;
        if(receipt->completed) {
            result=receipt->result;output.requested=receipt->requested;output.flags=receipt->flags;
            if(task->lookup_scope.armed)task->lookup_scope.cursor=receipt->cursor_after;
        } else {
            result=task_socket_message(task, file, message, message_flags, receiving, true, &output);
            if(result>=0) {
                receipt->completed=true;receipt->result=result;receipt->requested=output.requested;
                receipt->flags=output.flags;receipt->cursor_after=task->lookup_scope.cursor;
            }
        }
        if (result == KERNEL_SYSCALL_BLOCKED) return result;
#ifdef CONFIG_SWAP
        if (result == KERNEL_SYSCALL_SWAP_CONTENDED) return result;
#endif
        if (result < 0) { error = (int)result; break; }
        uint32_t message_length = (uint32_t)result;
        /* The operation precedes this store: EFAULT must not undo sent data,
         * consumed data or transferred SCM_RIGHTS descriptors. */
        uint64_t length_pointer = message + __builtin_offsetof(struct mmsghdr, msg_len);
#ifdef CONFIG_SWAP
        int prep=swap_prepare_user_range(task,length_pointer,sizeof(message_length),true);
        if(prep==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
#endif
        if (!user_range_writable(length_pointer, sizeof(message_length))) {
            error = -LINUX_EFAULT;
            break;
        }
        __builtin_memcpy((void *)(uintptr_t)length_pointer, &message_length, sizeof(message_length));
        ++state->count;
        state->lookup_cursor=task->lookup_scope.cursor;
        task->socket_io_deadline = 0;
        task->socket_io_timed = false;
        if (!receiving && (uint64_t)result < output.requested) break;
        /* Linux checks this timeout after receiving each message, not while
         * blocked inside recvmsg. SO_RCVTIMEO remains an independent timeout. */
        if (receiving && state->timeout_pointer && mmsg_update_timeout(state)) break;
        if (receiving && (output.flags & MSG_OOB)) break;
    }
    task_socket_cancel_receive(task);
    return mmsg_finish(task, file, error,meta,true);
}

/**
 * @brief Finish message counts before constructing a signal handler frame.
 * @param task Interrupted task, pinned under the kernel execution lock.
 * @param received Bytes already copied in the current WAITALL receive.
 * @return Batch count or error; all output goes through the target page tables.
 */
int64_t task_socket_mmsg_interrupt(struct task *task, uint64_t received)
{
    struct task_mmsg_state *state = &task->mmsg;
    if (!state->active) return -LINUX_EINTR;
    /* sock_intr_errno distinguishes SO_RCVTIMEO from an infinite wait. */
    int error = task->socket_io_timed ? -LINUX_EINTR : -512;
    if (state->receiving && received) {
        uint64_t message = state->vector + (uint64_t)state->count * sizeof(struct mmsghdr);
        uint32_t flags = task->socket_receive_flags;
        uint64_t no_control = 0;
        if (task->socket_receive_name) {
            uint32_t length = task->socket_receive_path_length;
            uint32_t copied = length < task->socket_receive_name_capacity ? length : task->socket_receive_name_capacity;
            if ((copied && user_copy_to_task(task, task->socket_receive_name, task->socket_receive_path, copied) < 0) ||
                user_copy_to_task(task, message + __builtin_offsetof(struct msghdr, msg_namelen), &length, sizeof(length)) < 0)
                return mmsg_finish(task, task->syscall_file, -LINUX_EFAULT,NULL,false);
        }
        if (user_copy_to_task(task, message + __builtin_offsetof(struct msghdr, msg_flags), &flags, sizeof(flags)) < 0 ||
            user_copy_to_task(task, message + __builtin_offsetof(struct msghdr, msg_controllen), &no_control, sizeof(no_control)) < 0)
            return mmsg_finish(task, task->syscall_file, -LINUX_EFAULT,NULL,false);
        uint64_t pointer = state->vector + (uint64_t)state->count * sizeof(struct mmsghdr) +
            __builtin_offsetof(struct mmsghdr, msg_len);
        uint32_t length = (uint32_t)received;
        if (user_copy_to_task(task, pointer, &length, sizeof(length)) < 0) error = -LINUX_EFAULT;
        else {
            ++state->count;
            if ((state->flags & (MSG_WAITFORONE | MSG_DONTWAIT)) ||
                (task->syscall_file && (task->syscall_file->flags & RELIEFOS_O_NONBLOCK)))
                error = -LINUX_EAGAIN;
            if (state->timeout_pointer && mmsg_update_timeout(state)) error = 0;
            if (state->count == state->length) error = 0;
        }
    }
    int64_t result = mmsg_finish(task, task->syscall_file, error,NULL,false);
    return result == -512 ? -LINUX_EINTR : result;
}

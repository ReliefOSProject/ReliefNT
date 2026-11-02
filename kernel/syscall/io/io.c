#include "../internal.h"
#include <reliefnt/swap.h>
#include <reliefnt/net_udp.h>
#include <reliefnt/net_packet.h>
#include <reliefnt/netlink.h>

/** @brief Identify INET datagrams which must retain whole-message semantics.
 * @param file Borrowed pinned OFD, nullable. @return True for UDP, raw or netlink. */
static bool scalar_inet_datagram(const struct task_file *file)
{
    return file && (file->flags & TASK_FILE_FLAG_SOCKET_INET) &&
        (file->kind == TASK_FILE_KIND_UDP || file->kind == TASK_FILE_KIND_PACKET ||
         file->kind == TASK_FILE_KIND_NETLINK);
}

/** @brief Identify a scalar buffer consumed by the staged regular-I/O path.
 * @param file Borrowed admitted descriptor, nullable. @return True for an
 * ordinary file or genuine block node; special devices keep their own checks.
 */
static bool scalar_staged_file(const struct task_file *file)
{
    if (!file || file->character_file || file->kind == TASK_FILE_KIND_SIGNALFD ||
        (file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_SOCKET_UNIX |
                        TASK_FILE_FLAG_SOCKET_INET | TASK_FILE_FLAG_EVENTFD |
                        TASK_FILE_FLAG_TIMERFD | TASK_FILE_FLAG_DEV_SHM |
                        TASK_FILE_FLAG_DEV_NULL))) return false;
    if (file->flags & TASK_FILE_FLAG_DEV_NODE) return (file->flags & TASK_FILE_FLAG_DEV_BLOCK) != 0;
    return file->node.type == RELIEFOS_FS_TYPE_FILE;
}

/** @brief Check a declared scalar range without faulting any page.
 * @param address First user byte. @param count Original syscall count.
 * @return True for an empty request or a nonwrapping user-aperture range.
 * The original count is checked before provider-specific transfer caps.
 */
static bool scalar_address_valid(uint64_t address,uint64_t count)
{
    return !count || (address>=RELIEFNT_USER_BASE && address<RELIEFNT_USER_TOP &&
                      count<=RELIEFNT_USER_TOP-address);
}

/** @brief Prepare a bounded synchronous scalar operand before provider entry.
 * @param task Current syscall owner. @param file Admitted OFD, nullable for
 * an implicit PTY. @param address User address. @param count Requested bytes.
 * @param writing True for input. @return Zero, EFAULT, or private contention.
 * Pipe and shared-memory callbacks consume at most 32 KiB, PTY and INET use
 * their existing smaller slice, and eventfd/timerfd consume one word. Shared
 * memory checks its full original provider extent for seals before writing.
 * A matching readiness wait is removed
 * before the timed prerequisite wait and is re-established by the backend on
 * retry. No DMA command or retained user pointer belongs to these callbacks.
 * Other provider classes retain their existing validation until separately
 * audited; ordinary files use their own owned staging path.
 */
static int scalar_prepare_user(struct task *task,const struct task_file *file,
                               uint64_t address,uint64_t count,bool writing)
{
    if(!scalar_address_valid(address,count))return -RELIEFOS_EFAULT;
    if (scalar_staged_file(file)) return 0;
    /* The current DRM event provider has no event payload; its empty read
     * must reach the provider without faulting an unused destination. */
    if (!writing && file && file->character_file && file->drm_file) return 0;
#ifdef CONFIG_SWAP
    /* Sound providers prepare actual frames or emitted events themselves,
     * before taking their PCM/timer locks. They must see empty queues before
     * touching an unused capacity and preserve accepted stream prefixes. */
    if (file && file->character_file &&
        (file->audio_file || file->audio_control_file || file->audio_mixer_file ||
         file->audio_timer_file || file->audio_oss_file)) return 0;
#endif
    if (file && !file->character_file &&
        ((file->flags & TASK_FILE_FLAG_DEV_NULL) ||
         ((file->flags & TASK_FILE_FLAG_DEV_NODE) && task_device_is(file,STORAGE_DEV_KIND_NULL))))
        return 0;
    uint64_t size=count;
    bool bounded=!file || (file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_SOCKET_INET
#ifdef CONFIG_SWAP
                                           | TASK_FILE_FLAG_DEV_SHM
#endif
                                           ));
#ifdef CONFIG_SWAP
    if(file && (file->flags & TASK_FILE_FLAG_DEV_SHM))size=(uint32_t)count;
#endif
    if (file && (file->flags & (TASK_FILE_FLAG_EVENTFD | TASK_FILE_FLAG_TIMERFD)) && count==8)
        bounded=true;
    if (bounded) {
        uint64_t limit=file && (file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_DEV_SHM)) ? 32768u :
                       file && (file->flags & (TASK_FILE_FLAG_EVENTFD | TASK_FILE_FLAG_TIMERFD)) ? 8u :
                       RELIEFOS_FS_IO_SLICE_BYTES;
        if(size>limit)size=limit;
        int rc=swap_prepare_user_range(task,address,size,!writing);
        if(rc==SWAP_STORAGE_CONTENDED) {
            if(task && task->waiting_queue)kernel_wait_queue_remove(task->waiting_queue,task);
            return KERNEL_SYSCALL_SWAP_CONTENDED;
        }
        if(rc<0)return -RELIEFOS_EFAULT;
    }
    return (writing ? user_range_ok(address,size) : user_range_writable(address,size)) ?
           0 : -RELIEFOS_EFAULT;
}


/**
 * @brief Run the shared write backend without unrelated legacy syscall locals.
 * @param a0 Descriptor number; entry may already pin its open description.
 * @param a1 User buffer, validated by the backend before accessing its bytes.
 * @param a2 Requested byte count; null writes accept up to Linux MAX_RW_COUNT.
 * @return Accepted byte count, negative errno, or internal blocking/retry result.
 * Null writes discard the full capped request without accessing its bytes.
 * Ordinary writes use the actual OFD provider independently of display text;
 * network datagrams use the staged socket send path as one complete message;
 * entry owns its lifetime through waits under execution ownership, without
 * graph/module locks spanning provider callbacks.
 */
int64_t syscall_io_write(uint64_t a0, uint64_t a1, uint64_t a2)
{
    struct task *task = sched_current_task();
    uint32_t request_len;
    int pty_stream;
    struct task_file *signal_file = task_file_for_io(task, (int32_t)a0);
    if (signal_file && (signal_file->flags & TASK_FILE_FLAG_PATH)) return -RELIEFOS_EBADF;
    if (signal_file && signal_file->kind == TASK_FILE_KIND_SIGNALFD) return -LINUX_EINVAL;
    if (signal_file && signal_file->kind == TASK_FILE_KIND_INOTIFY) return -LINUX_EBADF;
#ifdef CONFIG_SWAP
    if(signal_file && (signal_file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
        if(!scalar_address_valid(a1,a2))return -RELIEFOS_EFAULT;
        uint32_t request=a2>0x7ffff000u ? 0x7ffff000u : (uint32_t)a2;
        return task_socket_send_user(task,signal_file,a1,request);
    }
#endif
    if (scalar_inet_datagram(signal_file))
        return syscall_socket_dispatch(LINUX_SYS_SENDTO, a0, a1, a2, 0, 0, 0);
    int prep=scalar_prepare_user(task,signal_file,a1,a2,true);
    if(prep)return prep;
    {
        struct task_pty_fd *endpoint = task_pty_for_io(task, (int)a0);
        if (endpoint) {
            uint32_t access = task_pty_status(endpoint) & RELIEFOS_O_ACCMODE;
            if (access != RELIEFOS_O_WRONLY && access != RELIEFOS_O_RDWR) return -RELIEFOS_EBADF;
            if (!a2) return 0;
            uint32_t request = a2 > RELIEFOS_FS_IO_SLICE_BYTES
                                   ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
            if (endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER) {
                return pty_write_input(0, endpoint->pty_id,
                                       (const char *)(uintptr_t)a1, request);
            }
            int64_t change = pty_check_change(endpoint->pty_id, task->pid, 0);
            if (change) return change;
            return pty_write_output(endpoint->pty_id,
                                    (const char *)(uintptr_t)a1, request);
        }
    }
    {
        struct task_file *pipe_file = task_file_for_io(task, (int)a0);
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_PIPE)) {
            uint32_t request = a2 > 32768u ? 32768u : (uint32_t)a2;
            return task_pipe_write(pipe_file, (const void *)(uintptr_t)a1, request);
        }
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
            uint32_t request = a2 > 0x7ffff000 ? 0x7ffff000 : (uint32_t)a2;
            if (!user_range_ok(a1, request)) return -RELIEFOS_EFAULT;
            int ret = task_socket_write(pipe_file, (const void *)(uintptr_t)a1, request);
            if (ret == -RELIEFOS_EPIPE) sched_signal_user_task(task->pid, 13);
            return ret;
        }
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_SOCKET_INET)) {
            uint32_t request = a2 > RELIEFOS_FS_IO_SLICE_BYTES ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
            if (!user_range_ok(a1, request)) return -RELIEFOS_EFAULT;
            return task_inet_write(pipe_file, (const void *)(uintptr_t)a1, request);
        }
    }
    pty_stream = task->syscall_file ? -1 : task_pty_stream_for_fd(task, (int)a0);
    if (pty_stream == 1 || pty_stream == 2) {
        request_len = a2 > RELIEFOS_FS_IO_SLICE_BYTES
                          ? RELIEFOS_FS_IO_SLICE_BYTES
                          : (uint32_t)a2;
        return pty_write_output(task->pty_id, (const char *)(uintptr_t)a1, request_len);
    }
    /**
 * @brief A redirected stdio descriptor must be handled by the regular file path below. Only an unbound implicit PTY stream falls back to the kernel console; otherwise a pipe on fd 1/2 would be silently bypassed and its consumer would receive EOF/zero bytes.
 */
    if ((a0 == 1 || a0 == 2) &&
        (sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)a0)) == 0 &&
        !task_file_for_io(task, (int)a0)) {
        request_len = a2 > RELIEFOS_FS_IO_SLICE_BYTES
                          ? RELIEFOS_FS_IO_SLICE_BYTES
                          : (uint32_t)a2;
        console_write_len((const char *)(uintptr_t)a1, (size_t)request_len);
        return (int64_t)request_len;
    }
    struct task_file *file = task_file_for_io(task, (int)a0);
    if (!file) {
        return -RELIEFOS_EBADF;
    }
    if(file->character_file) {
        if(!file_can_write(file))return -RELIEFOS_EBADF;
        return character_file_write(file->character_file,task,file,(const void *)(uintptr_t)a1,
                                     task_device_write_length(file,a2));
    }
    if (file->flags & TASK_FILE_FLAG_EVENTFD) {
        uint64_t value;
        if (a2 != sizeof(uint64_t)) return -RELIEFOS_EINVAL;
        value = *(const uint64_t *)(uintptr_t)a1;
        if (value == UINT64_MAX) return -RELIEFOS_EINVAL;
        if (UINT64_MAX - 1ULL - file->aux < value) return -RELIEFOS_EAGAIN;
        file->aux += value;
        kernel_poll_readiness_notify();
        return sizeof(uint64_t);
    }
    if (file->flags & TASK_FILE_FLAG_DEV_SHM) {
#ifdef CONFIG_SWAP
        uint32_t request=(uint32_t)a2;
        int rc=task_shm_write_admit(file,request);if(rc)return rc;
        if(request>32768u)request=32768u;
        return task_shm_write(file,(const void *)(uintptr_t)a1,request);
#else
        if (a2 && !user_range_ok(a1, a2)) return -RELIEFOS_EFAULT;
        return task_shm_write(file, (const void *)(uintptr_t)a1, (uint32_t)a2);
#endif
    }
    /* A null node can carry DEV_NODE and DEV_NULL together, or only DEV_NODE.
     * Handle it before device chunking; discarding bytes needs no I/O slice. */
    if ((file->flags & TASK_FILE_FLAG_DEV_NULL) ||
        task_device_is(file, STORAGE_DEV_KIND_NULL)) {
        if (!file_can_write(file)) return -RELIEFOS_EBADF;
        return (int64_t)(a2 > 0x7ffff000u ? 0x7ffff000u : a2);
    }
    if (file->flags & TASK_FILE_FLAG_DEV_NODE) {
        if (!file_can_write(file)) return -RELIEFOS_EBADF;
        if (file->flags & TASK_FILE_FLAG_DEV_BLOCK) {
            /* open(2) checked device DAC; the writable descriptor authorizes I/O. */
            return syscall_regular_io(task,file,a1,a2,0,true,false);
        }
        request_len = task_device_write_length(file, a2);
        {
            int result = task_device_write(task, file, (const void *)(uintptr_t)a1,
                                           request_len);
            if (result > 0 && (file->flags & TASK_FILE_FLAG_DEV_BLOCK)) {
                file->offset += (uint32_t)result;
            }
            return result;
        }
    }
    if (!file_can_write(file)) {
        return -RELIEFOS_EBADF;
    }
    if (file->node.type == RELIEFOS_FS_TYPE_DIR) {
        return -RELIEFOS_EISDIR;
    }
    if (file->node.type != RELIEFOS_FS_TYPE_FILE) {
        return -RELIEFOS_EBADF;
    }
    return syscall_regular_io(task, file, a1, a2, 0, true, false);
}

/**
 * @brief Run the shared read backend, preserving network datagram boundaries.
 * @param a0 Descriptor number; entry may already pin its open description.
 * @param a1 User buffer, validated by the backend before accessing its bytes.
 * @param a2 Requested byte count, subject to each backend's existing limits.
 * @return Byte count, negative errno (including EISDIR for directories), or
 * internal retry. Ordinary reads use the actual OFD provider/open capture;
 * diagnostic paths never select an inode. Entry owns the OFD through waits,
 * under execution ownership without graph/module locks across callbacks.
 */
int64_t syscall_io_read(uint64_t a0, uint64_t a1, uint64_t a2)
{
    /* Standalone scalar-I/O fixtures may omit the anonymous inotify provider. */
    extern int64_t task_inotify_read(struct task *, struct task_file *, uint64_t, uint64_t) __attribute__((weak));
    struct task *task = sched_current_task();
    struct task_file *file;
    uint32_t request_len;
    int pty_stream;
    file = task_file_for_io(task, (int32_t)a0);
    if (file && (file->flags & TASK_FILE_FLAG_PATH)) return -RELIEFOS_EBADF;
    if (file && file->kind == TASK_FILE_KIND_SIGNALFD) return task_signalfd_read(task, file, a1, a2);
    if (file && file->kind == TASK_FILE_KIND_INOTIFY)
        return task_inotify_read ? task_inotify_read(task, file, a1, a2) : -RELIEFOS_ENOSYS;
#ifdef CONFIG_SWAP
    if(file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
        if(!scalar_address_valid(a1,a2))return -RELIEFOS_EFAULT;
        uint32_t request=a2>RELIEFOS_FS_IO_SLICE_BYTES?RELIEFOS_FS_IO_SLICE_BYTES:(uint32_t)a2;
        return task_socket_receive_user(task,file,a1,request,0,0,0);
    }
#endif
    if (scalar_inet_datagram(file)) {
        if (!a2) return 0;
        return syscall_socket_dispatch(LINUX_SYS_RECVFROM, a0, a1, a2, 0, 0, 0);
    }
    int prep=scalar_prepare_user(task,file,a1,a2,false);
    if(prep)return prep;
    if(file && file->character_file) {
        if(!file_can_read(file))return -RELIEFOS_EBADF;
        uint32_t count=a2>RELIEFOS_FS_IO_SLICE_BYTES ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
        return character_file_read(file->character_file,task,file,(void *)(uintptr_t)a1,count);
    }
    {
        struct task_pty_fd *endpoint = task_pty_for_io(task, (int)a0);
        if (endpoint) {
            uint32_t access = task_pty_status(endpoint) & RELIEFOS_O_ACCMODE;
            if (access != RELIEFOS_O_RDONLY && access != RELIEFOS_O_RDWR) return -RELIEFOS_EBADF;
            if (!a2) return 0;
            uint32_t request = a2 > RELIEFOS_FS_IO_SLICE_BYTES
                                   ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
            if (endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER) {
                if ((task_pty_status(endpoint) & RELIEFOS_O_NONBLOCK) &&
                    !pty_output_available(endpoint->pty_id) &&
                    !pty_is_hungup(endpoint->pty_id)) {
                    return -RELIEFOS_EAGAIN;
                }
                return pty_read_output(0, endpoint->pty_id,
                                       (char *)(uintptr_t)a1, request);
            }
            if ((task_pty_status(endpoint) & RELIEFOS_O_NONBLOCK) &&
                !pty_input_available(endpoint->pty_id) &&
                !pty_is_hungup(endpoint->pty_id)) {
                int64_t change = pty_check_change(endpoint->pty_id, task->pid, 21);
                return change ? change : -RELIEFOS_EAGAIN;
            }
            int64_t change = pty_check_change(endpoint->pty_id, task->pid, 21);
            if (change) return change;
            return pty_read_input(endpoint->pty_id,
                                  (char *)(uintptr_t)a1, request);
        }
    }
    {
        struct task_file *pipe_file = task_file_for_io(task, (int)a0);
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_PIPE)) {
            uint32_t request = a2 > 32768u ? 32768u : (uint32_t)a2;
            return task_pipe_read(pipe_file, (void *)(uintptr_t)a1, request);
        }
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
            uint32_t request = a2 > RELIEFOS_FS_IO_SLICE_BYTES ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
            if (!user_range_ok(a1, request)) return -RELIEFOS_EFAULT;
            return task_socket_read(pipe_file, (void *)(uintptr_t)a1, request);
        }
        if (pipe_file && (pipe_file->flags & TASK_FILE_FLAG_SOCKET_INET)) {
            uint32_t request = a2 > RELIEFOS_FS_IO_SLICE_BYTES ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
            if (!user_range_ok(a1, request)) return -RELIEFOS_EFAULT;
            return task_inet_read(pipe_file, (void *)(uintptr_t)a1, request);
        }
    }
    pty_stream = task->syscall_file ? -1 : task_pty_stream_for_fd(task, (int)a0);
    if (pty_stream == 0) {
        uint32_t count=a2>RELIEFOS_FS_IO_SLICE_BYTES ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
        return pty_read_input(task->pty_id, (char *)(uintptr_t)a1, count);
    }
    file = task_file_for_io(task, (int)a0);
    if (!file) {
        return -RELIEFOS_EBADF;
    }
    if (file->flags & TASK_FILE_FLAG_EVENTFD) {
        uint64_t value;
        if (a2 != sizeof(uint64_t)) return -RELIEFOS_EINVAL;
        if (!file->aux) return -RELIEFOS_EAGAIN;
        value = file->aux2 ? 1 : file->aux;
        file->aux -= value;
        *(uint64_t *)(uintptr_t)a1 = value;
        kernel_poll_readiness_notify();
        return sizeof(uint64_t);
    }
    if (file->flags & TASK_FILE_FLAG_TIMERFD) {
        struct task_timerfd *timer = (struct task_timerfd *)(uintptr_t)file->aux;
        uint64_t value;
        if (a2 != sizeof(uint64_t)) return -RELIEFOS_EINVAL;
        task_timerfd_update(timer);
        if (!timer || !timer->expirations) return -RELIEFOS_EAGAIN;
        value = timer->expirations;
        timer->expirations = 0;
        *(uint64_t *)(uintptr_t)a1 = value;
        return sizeof(uint64_t);
    }
    if (file->flags & TASK_FILE_FLAG_DEV_SHM) {
#ifdef CONFIG_SWAP
        uint32_t request=(uint32_t)a2;if(request>32768u)request=32768u;
        return task_shm_read(file,(void *)(uintptr_t)a1,request);
#else
        if (a2 && !user_range_writable(a1, a2)) return -RELIEFOS_EFAULT;
        return task_shm_read(file, (void *)(uintptr_t)a1, (uint32_t)a2);
#endif
    }
    if (file->flags & TASK_FILE_FLAG_DEV_NODE) {
        if (!file_can_read(file)) return -RELIEFOS_EBADF;
        if (file->flags & TASK_FILE_FLAG_DEV_BLOCK) {
            return syscall_regular_io(task,file,a1,a2,0,false,false);
        }
        request_len = a2 > RELIEFOS_FS_IO_SLICE_BYTES
                          ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)a2;
        {
            int result = task_device_read(task, file, (void *)(uintptr_t)a1,
                                          request_len);
            if (result > 0 && (file->flags & TASK_FILE_FLAG_DEV_BLOCK)) {
                file->offset += (uint32_t)result;
            }
            return result;
        }
    }
    if (file->flags & TASK_FILE_FLAG_DEV_NULL) {
        if (!file_can_read(file)) {
            return -RELIEFOS_EBADF;
        }
        return 0;
    }
    /* Every ordinary provider, including proc/sysfs, follows its retained
     * inode and optional open-time capture. Linux read(2) never enumerates
     * directories; getdents owns that separate record ABI. */
    if (!file_can_read(file)) return -RELIEFOS_EBADF;
    if (file->node.type == RELIEFOS_FS_TYPE_DIR) return -RELIEFOS_EISDIR;
    if (file->node.type != RELIEFOS_FS_TYPE_FILE) return -RELIEFOS_EBADF;
    return syscall_regular_io(task, file, a1, a2, 0, false, false);
}

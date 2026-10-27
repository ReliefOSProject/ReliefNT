/* Internal syscall helpers shared by the syscall category translation units. */
#ifndef RELIEFNT_SYSCALL_INTERNAL_H
#define RELIEFNT_SYSCALL_INTERNAL_H
#include <stdint.h>
struct vfs_namespace;
struct vfs_lookup_result;
#include <stdbool.h>

struct task;
struct task_file;
struct vfs_path;
struct trap_frame;
struct task_lookup_path;
struct storage_inode_ref;
/** @brief Cancel a suspended two-descriptor copy before retiring its OFDs.
 * @param task Owner under execution ownership, nullable.
 * Detaches state and unlinks its wait record first, then drains any provider
 * wait before freeing staging bytes,
 * releases its offset reservations and both canonical OFDs. Final puts may
 * perform I/O; no scheduler/graph/module spinlock may span this operation.
 */
void task_transfer_cancel(struct task *task);
/** @brief Retire a suspended transfer when a different invocation replaces it.
 * @param task Calling task under execution ownership, nullable.
 * @param frame Current syscall registers, required with task.
 * Matching meaningful arguments keep original descriptors/offset snapshots;
 * RT_SIGRETURN follows the common suspended-call policy. No allocation or new
 * I/O; cancellation may drain old I/O before primary descriptor admission.
 */
void task_transfer_enter(struct task *task,const struct trap_frame *frame);
/** @brief Preserve accepted copy progress before signal cancellation.
 * @param task Current execution-owned task, nullable.
 * @return Accepted bytes, zero before any output, or EFAULT publishing changed
 * user offset mappings. Drains an outstanding provider wait before publishing
 * offsets; never rereads their input values. Retains the state/OFDs for common
 * cancellation. No scheduler/graph/module spinlock may span this operation.
 */
int64_t task_transfer_interrupt(struct task *task);
/** @brief Prepare stable lookup operands for a path syscall or its I/O retry.
 * @param task Current task under execution ownership, nullable.
 * @param frame Borrowed current syscall registers, required with a task.
 * Matching number/arguments reset the operand cursor while retaining copied
 * names and exact root/dirfd objects. A different call retires former operands.
 * Non-path calls allocate/pin nothing. No scheduler/module lock may span final
 * retirement callbacks; this function performs no new filesystem I/O.
 */
void task_lookup_scope_begin(struct task *task,const struct trap_frame *frame);
/** @brief Obtain this lookup's retry slot, with no fixed operand count limit.
 * @param task Borrowed task under execution ownership, nullable.
 * @param out Receives a borrowed empty/saved operand, or NULL for unscoped use.
 * @return Zero or negative errno. Extra slots may allocate; saved names/paths
 * remain owned by the task until task_lookup_scope_cancel. The caller populates
 * an empty slot only after validation and before entering retryable VFS I/O.
 */
int task_lookup_scope_next(struct task *task,struct task_lookup_path **out);
/** @brief Retire all copied lookup inputs and held directory objects once.
 * @param task Owner under execution ownership, nullable; empty scopes are cheap.
 * Detaches state before releasing references, so signals, changed calls, exit
 * and final syscall return share one cleanup. Final backend puts may perform
 * I/O; caller holds no scheduler, module-table or graph spinlock.
 */
void task_lookup_scope_cancel(struct task *task);
/** @brief Resolve a kernel-owned at-path using actual task root/cwd/OFD objects.
 * @param ns Shared live namespace under execution ownership.
 * @param task Borrowed current task with initialized root/cwd references.
 * @param dirfd Linux descriptor or AT_FDCWD, ignored for absolute input.
 * @param raw Borrowed NUL-terminated kernel input, bounded by VFS_PATH_MAX.
 * @param allow_empty Permit an empty input to reference cwd or the exact OFD.
 * Anonymous non-path descriptors require the calling syscall's separate OFD
 * operation; this path-only interface never invents names for them.
 * @param real_ids Select real credentials for access(2) directory search.
 * @param lookup_flags VFS_LOOKUP_FOLLOW/PARENT/MISSING policy.
 * @param out Empty result owning one path on success; parent mode also returns
 * the unresolved component and trailing-slash state.
 * @return Zero or negative errno with no references transferred on error.
 * Directory search checks run before consuming components, including dot-dot.
 * An active syscall scope owns an independent input/root/dirfd snapshot across
 * EAGAIN, retiring it through task_release_syscall_file on final return/cancel.
 * No pathname display cache or global text-prefix routing participates.
 */
int resolve_kernel_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                     const char *raw,bool allow_empty,bool real_ids,
                                     uint32_t lookup_flags,struct vfs_lookup_result *out);

struct task_user_text {const char *text;char *allocation;};
/** @brief Copy a counted binary user operand once across matching retries.
 * @param task Borrowed caller with optional active lookup scope.
 * @param user_ptr Readable user bytes, checked for the entire counted range.
 * @param length Byte count from one through INT32_MAX; each caller validates its own ABI limit.
 * @param out Empty output borrowing scoped bytes or owning allocation when
 * unscoped; caller always kernel_free(out->allocation). No NUL is required.
 * @return Zero or negative errno. Embedded NUL bytes survive unchanged;
 * scoped storage belongs to common cancellation. No inode I/O or locks occur.
 */
int task_lookup_user_bytes(struct task *task,uint64_t user_ptr,uint32_t length,struct task_user_text *out);
/** @brief Copy a bounded non-path string once across syscall I/O retries.
 * @param task Borrowed caller with optional active lookup scope.
 * @param user_ptr User string address; checked through its terminating NUL.
 * @param maximum Capacity including NUL, from one through VFS_PATH_MAX.
 * @param allow_null Treat a NULL address as an empty string when true.
 * @param out Empty output borrowing text from the active scope, or owning
 * allocation when unscoped; caller always kernel_free(out->allocation).
 * @return Zero or negative errno. The task owns an active scope's copy until
 * common cancellation. Matching retries never reread changed/unmapped user
 * text. No namespace lookup, inode reference or lock is acquired here.
 */
int task_lookup_user_text(struct task *task,uint64_t user_ptr,uint32_t maximum,
                            bool allow_null,struct task_user_text *out);
/** @brief Exclude writers from an actual executable operand through retries.
 * @param task Borrowed caller with an optional active lookup scope.
 * @param path Borrowed actual regular inode; caller authorizes execution.
 * @param temporary Required output, NULL for scope-owned guards. Unscoped
 * callers receive one inode/deny reference and must allow_write then put it
 * before releasing their own path/provider pin.
 * @return Zero or negative type/allocation/ETXTBSY/overflow errno. Scoped
 * guards retain the path and survive matching retries without double-denial;
 * common cancellation allows writes before dropping inode/provider pins.
 * Execution ownership required; no graph/module lock spans backing callbacks.
 */
int task_lookup_exec_guard(struct task *task,const struct vfs_path *path,
                             struct storage_inode_ref **temporary);

/** @brief Borrow a live numeric descriptor slot without promoting its OFD.
 * @param task Execution-owned task/fd table, nullable. @param fd Linux descriptor number.
 * @return Borrowed live slot or NULL, including stdio/grown tables. No reference
 * is acquired, no allocation/lock/wait occurs; retain the canonical description
 * before any operation that can suspend ownership or permit fd-table reuse.
 */
struct task_file *task_descriptor_for_fd(struct task *task, int fd);
/**
 * @brief Reserve the lowest available descriptor and initialize its table-owned slot.
 * @param task Required live table owner under the execution transaction.
 * @param minimum Inclusive descriptor lower bound.
 * @param slot Required writable borrowed-slot output, assigned only on success.
 * @return Descriptor number or negative validation/resource errno without consuming a slot on failure.
 * Task context, serialized against table mutation; storage may grow. Caller constructs the OFD before publication or rolls back with task_discard_file_fd.
 */
int task_allocate_fd(struct task *task, int minimum, struct task_file **slot);
/**
 * @brief Roll back a reserved descriptor and its partially constructed resources.
 * @param task Required table owner under execution ownership.
 * @param fd Reserved descriptor to consume; stdio closure masks are updated.
 * Task context, serialized against close/dup. Releases record locks and file resources; final callbacks may run, so no table/provider spinlock may span this call.
 */
void task_discard_file_fd(struct task *task, int fd);
/**
 * @brief Install a descriptor alias retaining the original canonical OFD.
 * @param destination Required unused writable table slot, distinct from source.
 * @param source Borrowed live descriptor or canonical OFD stable against close.
 * @return Zero, EBADF for invalid input, or ENOMEM if canonical promotion fails.
 * Execution-owned task context; promotion may allocate. Success transfers a table-owned reference into destination, later consumed by normal close.
 */
int task_file_reference(struct task_file *destination, struct task_file *source);
/**
 * @brief Acquire a transient canonical OFD reference, promoting inline state if needed.
 * @param source Borrowed live descriptor/OFD, stable against concurrent close.
 * @return Owned canonical OFD reference or NULL for invalid input or promotion OOM.
 * Execution-owned task context; promotion may allocate. Release exactly once with task_file_put; the reference preserves the original provider and backing.
 */
struct task_file *task_file_get(struct task_file *source);

/** @brief Bind an authorized held path to the canonical open-file description.
 * @param file Live descriptor or canonical OFD under execution ownership.
 * @param path Borrowed path; NULL clears the previous binding.
 * Takes the new reference before dropping the old one, without allocation.
 * Dup/fork/in-flight/VMA references share this one OFD-owned mount lifetime;
 * final close releases it after file/device callbacks. No graph lock is held.
 */
void task_file_bind_path(struct task_file *file,const struct vfs_path *path);
/**
 * @brief Consume one transient OFD reference and perform final resource cleanup.
 * @param description Owned canonical reference from task_file_get, or NULL.
 * Execution-owned task context, serialized with descriptor mutations. Final put detaches epoll watchers and runs original file/provider cleanup before heap release; no registry spinlock spans those callbacks.
 */
void task_file_put(struct task_file *description);
/**
 * @brief Resolve the stable OFD bound to the current syscall or descriptor.
 * @param task Current task; NULL returns NULL.
 * @param fd Linux descriptor number whose in-flight syscall pin may override lookup.
 * @return Borrowed stable file description, or NULL when the descriptor is not a file.
 * @note Caller serializes with descriptor mutation; the returned pointer is not retained.
 */
struct task_file *task_file_for_io(struct task *task, int fd);

#include <reliefnt/sched.h>

/**
 * @brief Transfer regular-file or genuine block-node data, retaining progress across retries.
 * @param task Current task, with dispatcher-owned retry state.
 * @param file Authorized canonical OFD retaining its actual provider path.
 * @param buffer User buffer already validated by the dispatcher for count bytes.
 * @param count Requested byte count; capped at Linux MAX_RW_COUNT.
 * @param position Byte offset for positional I/O; otherwise use the shared file offset.
 * @param writing Whether to write rather than read.
 * @param positional Whether to preserve the shared file offset.
 * @return Bytes transferred, negative errno, or -EAGAIN with saved retry progress.
 * Execution-owned; no graph/module lock spans I/O. Regular reads/writes,
 * append metadata and sync use the same retained provider, preserving the
 * OFD cursor and actual inode after diagnostic names change. Genuine block
 * nodes retain their validated block-service path.
 */
int64_t syscall_regular_io(struct task *task, struct task_file *file,
                           uint64_t buffer, uint64_t count, uint64_t position,
                           bool writing, bool positional);

/** @brief Execute native SysV semaphore calls under the kernel execution lock. */
int64_t syscall_sysv_sem(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
/** @brief Cancel an in-flight semaphore operation without applying persistent undo records. */
void task_sysv_sem_cancel(struct task *task);
/** @brief Check whether completion or timeout precedes signal delivery. */
bool task_sysv_sem_ready(const struct task *task);

/** @brief Execute native SysV message queue operations under the execution lock. */
int64_t syscall_sysv_msg(uint64_t number, uint64_t a0, uint64_t a1,
                         uint64_t a2, uint64_t a3, uint64_t a4);
/** @brief Release a task's pending SysV operation on completion, signal, exec or exit. */
void task_sysv_msg_cancel(struct task *task);

/** @brief Execute process_vm_readv/writev against a task pinned by the execution lock. */
int64_t syscall_process_vm(int32_t pid, uint64_t local, uint64_t local_count,
                           uint64_t remote, uint64_t remote_count, uint64_t flags, bool write);

int64_t syscall_signalfd(int32_t fd, uint64_t mask, uint64_t size, uint32_t flags);
short task_signalfd_poll(struct task *task, const struct task_file *file);
int64_t task_signalfd_read(struct task *task, struct task_file *file, uint64_t buffer, uint64_t count);
int64_t task_signalfd_readv(struct task *task, struct task_file *file, uint64_t vectors,
                           uint64_t count, uint32_t flags);

struct socket_message_result {
    uint64_t requested;
    uint32_t flags;
};
/** @brief Transfer one message through an admitted socket description.
 * @param task Borrowed caller under execution ownership.
 * @param file Original retained socket OFD across blocking retries.
 * @param message User native msghdr input/output address.
 * @param flags Native MSG flags.
 * @param receiving Select recvmsg instead of snapshotting outgoing metadata.
 * @param batch Whether a message batch owns progress and partial completion.
 * @param result Required requested-byte and result-flag output.
 * @return Bytes, negative errno or internal blocking sentinel. Outgoing
 * header/iovecs/address/path remain scope-owned until common cancellation;
 * no graph/module spinlock spans callbacks. Provider paths stay pinned.
 */
int64_t task_socket_message(struct task *task, struct task_file *file, uint64_t message,
                            uint32_t flags, bool receiving, bool batch,
                            struct socket_message_result *result);
/** @brief Read and clear, or set, a socket's deferred receive error. */
int task_socket_message_error(struct task_file *file, int error, bool setting);
/** @brief Iterate a message batch while retaining its current operand cursor.
 * @param receiving Select recvmmsg rather than sendmmsg.
 * @param fd Signed user descriptor, admitted by the common dispatcher.
 * @param vector User native mmsghdr array.
 * @param length Message count, capped to UIO_MAXIOV for sending only.
 * @param flags Native MSG flags, validated by the socket implementation.
 * @param timeout User recvmmsg timespec address, or zero.
 * @return Completed message count, negative errno or internal blocking
 * sentinel. Execution ownership required; current-message operands survive
 * retries without repeating completed messages. No lock spans socket I/O.
 */
int64_t syscall_socket_mmsg(bool receiving, int fd, uint64_t vector,
                            uint32_t length, uint32_t flags, uint64_t timeout);
/** @brief Complete an interrupted message batch using the target task's address space. */
int64_t task_socket_mmsg_interrupt(struct task *task, uint64_t received);

int64_t syscall_nanosleep(int32_t clock, uint32_t flags, uint64_t request, uint64_t remaining);
int64_t syscall_itimer(bool setting, int32_t which, uint64_t value, uint64_t old_value);
int64_t syscall_timer_create(uint64_t clockid, uint64_t sevp, uint64_t timerid);
int64_t syscall_timer_delete(uint64_t timerid);
int64_t syscall_timer_settime(uint64_t timerid, uint64_t flags, uint64_t value, uint64_t old_value);
int64_t syscall_timer_gettime(uint64_t timerid, uint64_t value);
int64_t syscall_timer_getoverrun(uint64_t timerid);
int64_t syscall_rt_sigtimedwait(uint64_t mask, uint64_t info, uint64_t timeout, uint64_t sigset_size);

#define TASK_PIPE_CAP 65536u

/**
 * @brief Resolve a table descriptor to its canonical open-file description.
 * @param task Borrowed table owner.
 * @param fd Descriptor number to resolve.
 * @return Borrowed canonical OFD or NULL; no reference is acquired.
 * Execution-owned task context. Keep table ownership through use or take an independent task_file_get reference before permitting concurrent close.
 */
struct task_file *task_file_for_fd(struct task *task, int fd);
/**
 * @brief Check the retained file access mode without acquiring a reference.
 * @param file Borrowed stable OFD; NULL preserves the legacy read-only default.
 * @return One for RDONLY/RDWR, zero otherwise.
 * Execution-owned task context; no allocation, locks, user access or ownership transfer.
 */
int file_can_read(const struct task_file *file);
/**
 * @brief Check whether the retained file access mode permits writes.
 * @param file Borrowed stable OFD, nullable.
 * @return One for WRONLY/RDWR, zero otherwise, including NULL.
 * Execution-owned task context; no allocation, locks, user access or ownership transfer.
 */
int file_can_write(const struct task_file *file);
int storage_errno(int ret);
/**
 * @brief Open a named FIFO, retaining a private endpoint across blocking retries.
 * @param task Current task under the kernel execution lock.
 * @param node Resolved FIFO inode, or NULL when resuming its open.
 * @param flags Linux open flags.
 * @param path Resolved absolute path for fd metadata.
 * @return Descriptor, negative errno, or KERNEL_SYSCALL_BLOCKED for an interruptible wait.
 */
int task_fifo_open(struct task *task, const struct storage_node *node, uint32_t flags, const char *path);
/**
 * @brief Cancel an unpublished FIFO endpoint on signal, exit or failed open.
 * @param task Task whose pending FIFO open should be released; may be NULL.
 */
void task_fifo_cancel(struct task *task);
/** @brief Acquire endpoint counts for a newly owned pipe description.
 * @param file Pinned pipe description under execution ownership.
 */
void task_pipe_retain(struct task_file *file);
/** @brief Release an endpoint, wake peers and reclaim the final pipe and ring.
 * @param file Retiring owned pipe description under execution ownership.
 */
void task_pipe_release(struct task_file *file);
/** @brief Read buffered bytes, EOF or an interruptible empty-pipe retry.
 * @param file Pinned readable description under execution ownership.
 * @param buffer Validated writable destination for length bytes.
 * @param length Maximum byte count.
 * @return Byte count, zero at EOF, or negative EBADF/EAGAIN.
 */
int task_pipe_read(struct task_file *file, void *buffer, uint32_t length);
/** @brief Write pipe bytes atomically up to PIPE_BUF and signal absent readers.
 * @param file Pinned writable description under execution ownership.
 * @param buffer Validated source for length bytes.
 * @param length Requested byte count.
 * @return Bytes accepted or negative EBADF/EPIPE/EAGAIN; EPIPE also raises SIGPIPE.
 */
int task_pipe_write(struct task_file *file, const void *buffer, uint32_t length);
/** @brief Query readiness without parking; include terminal HUP/ERR conditions.
 * @param file Pinned description under execution ownership.
 * @param events Requested Linux poll bits.
 * @return Ready event bits or POLLNVAL for a stale/nonpipe description.
 */
short task_pipe_poll(const struct task_file *file, short events);
/** @brief Query or resize one pipe's byte capacity under execution ownership.
 * @param file Borrowed pipe description, readable or writable.
 * @param setting True for F_SETPIPE_SZ, false for F_GETPIPE_SZ.
 * @param bytes Requested unsigned Linux int capacity; ignored for queries.
 * @return Actual page-rounded capacity or negative EBADF/EINVAL/EBUSY/EPERM/ENOMEM.
 */
int64_t task_pipe_capacity(struct task_file *file, bool setting, uint32_t bytes);
/**
 * @brief Receive through the original pinned Internet socket without changing OFD flags.
 * @param file Borrowed live socket retained by syscall admission.
 * @param buffer Writable validated length-byte destination.
 * @param length Maximum bytes, zero allowed.
 * @param flags Stream PEEK/DONTWAIT or datagram/packet receive flags.
 * @return Bytes, EOF, negative errno or the blocking sentinel. Unsupported
 * stream flags fail before consumption. Caller holds execution ownership;
 * peeking preserves the queue and per-call DONTWAIT never changes O_NONBLOCK.
 */
int task_inet_recv(struct task_file *file, void *buffer, uint32_t length, uint32_t flags);
/**
 * @brief Read available socket bytes using the shared descriptor's ordinary policy.
 * @param file Original syscall-pinned Internet socket description.
 * @param buffer Writable validated length-byte destination.
 * @param length Maximum bytes, zero allowed.
 * @return Bytes, EOF, negative errno or the blocking sentinel. Execution-owned;
 * no descriptor flags or ownership are transferred by this wrapper.
 */
int task_inet_read(struct task_file *file, void *buffer, uint32_t length);
int task_net_control(struct task_file *file, uint32_t request, uint64_t address);
int task_inet_write(struct task_file *file, const void *buffer, uint32_t length);
short task_inet_poll(const struct task_file *file, short events);
void task_inet_retain(struct task_file *file);
void task_inet_release(struct task_file *file);
void task_socket_retain(struct task_file *file);
void task_socket_collect(void);
void task_socket_release(struct task_file *file);
int task_socket_read(struct task_file *file, void *buffer, uint32_t length);
int task_socket_write(struct task_file *file, const void *buffer, uint32_t length);
struct iovec;
int task_socket_vector(struct task_file *file, const struct iovec *vectors,
                       uint32_t count, uint64_t total, bool writing);
short task_socket_poll(const struct task_file *file, short events);
/**
 * @brief Returns the receive-generation counter for a UNIX stream socket.
 * @param file Open socket file description to inspect.
 * @return Monotonic receive generation, or zero for non-UNIX sockets.
 */
uint64_t task_socket_event_generation(const struct task_file *file);
/**
 * @brief Handle Unix socket ioctls with native usercopy and error semantics.
 * @param file Retained Unix socket open file description.
 * @param request Linux ioctl number.
 * @param address User result pointer.
 * @return Zero or a negative Linux errno.
 */
int task_socket_ioctl(struct task_file *file, uint64_t request, uint64_t address);
int64_t syscall_socket_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
/** @brief Create two resource-backed pipe endpoints with complete failure rollback.
 * @param user_ptr User destination for two int descriptors, checked writable.
 * @return Zero or a negative descriptor/allocation/EFAULT error; execution ownership required.
 */
int syscall_ipc_pipe(uint64_t user_ptr);
int syscall_ipc_pipe2(uint64_t user_ptr, uint64_t flags);
/** @brief Retain the seal state of an anonymous shmem description.
 * @param file Live description under execution ownership, nullable.
 */
void task_shm_retain(struct task_file *file);
/** @brief Release seal state; OFD and VMA inode pins own the backing pages.
 * @param file Retiring description under execution ownership, nullable.
 */
void task_shm_release(struct task_file *file);
/** @brief Attach an owned anonymous tmpfs inode and independent seal state.
 * @param file Fresh description; ownership transfers only on success.
 * @param create_flags Validated Linux MFD flags.
 * @return Zero or negative EINVAL, ENOMEM, ENFILE or backing-store error.
 */
int task_shm_attach(struct task_file *file, uint32_t create_flags);
/** @brief Open an independently positioned OFD for an authorized anonymous path.
 * @param file Fresh unpublished description with validated open flags; no inode/path yet.
 * @param path Caller-held anonymous target; not transferred.
 * @return Zero or negative EINVAL/ENOENT/ENOMEM/ETXTBSY/backing error, without
 * partially attaching resources on failure. Caller checks inode permissions
 * before entry. Execution transaction required. Seals and backing object remain
 * shared; writer leases and offsets belong to each independent OFD.
 */
int task_shm_reopen(struct task_file *file,const struct vfs_path *path);
/** @brief Set the shared anonymous inode's diagnostic memfd label.
 * @param file Attached unpublished shmem OFD under execution ownership.
 * @param name Borrowed kernel-owned basename, required even when empty.
 * @param length Basename byte count, at most NAME_MAX minus the memfd prefix.
 * @return Zero or EINVAL/EBADF; no allocation. This label is never a path lookup
 * target. Independent reopened OFDs inherit it from their shared backing state.
 */
int task_shm_set_name(struct task_file *file,const char *name,size_t length);
/** @brief Change logical size sparsely and revoke pages removed by shrink.
 * @param file Writable shmem description under execution ownership.
 * @param size New nonnegative signed-off_t size in bytes.
 * @return Zero or negative EINVAL, EBADF, EPERM or backing-store error.
 */
int task_shm_truncate(struct task_file *file, uint64_t size);
/** @brief Read bytes or sparse zeros and advance the shared OFD offset.
 * @param file Pinned description under execution ownership.
 * @param buffer Validated destination for length bytes.
 * @param length Maximum byte count.
 * @return Bytes read, zero at EOF, or a negative error.
 */
int task_shm_read(struct task_file *file, void *buffer, uint32_t length);
/** @brief Write sparse pages and preserve partial progress on allocation failure.
 * @param file Pinned writable description under execution ownership.
 * @param buffer Validated source for length bytes.
 * @param length Requested byte count.
 * @return Bytes written or negative EBADF, EFAULT, EFBIG, EPERM or backing error.
 */
int task_shm_write(struct task_file *file, const void *buffer, uint32_t length);
/** @brief Check memfd mapping seals and restrict later protection upgrades.
 * @param file Pinned shmem description under execution ownership.
 * @param flags Linux MAP flags. @param prot Requested PROT bits.
 * @param max_prot In/out permitted protection mask, nullable.
 * @return Zero or negative EBADF/EPERM; private mappings retain COW semantics.
 */
int task_shm_mmap_check(const struct task_file *file, uint32_t flags, uint32_t prot, uint32_t *max_prot);
/** @brief Read the inode-wide monotonic seal set.
 * @param file Pinned description under execution ownership, nullable.
 * @return Seal mask or negative EINVAL/EBADF.
 */
int64_t task_shm_get_seals(const struct task_file *file);
/** @brief Add seals atomically after access and live shared-mapping checks.
 * @param file Writable shmem description under execution ownership.
 * @param seals Known Linux seal bits to add; existing bits remain set.
 * @return Zero or negative EPERM, EINVAL, EBADF or EBUSY; no change on failure.
 */
int task_shm_add_seals(struct task_file *file, uint32_t seals);
int syscall_fs_owns(uint64_t number);
int64_t syscall_fs_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                            uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int syscall_ipc_owns(uint64_t number);
int64_t syscall_ipc_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
/**
 * @brief Identify the GPU device ioctl boundary.
 * @param number Linux syscall number.
 * @param a1 Ioctl request number.
 * @return Nonzero if this boundary owns the request.
 */
int syscall_gpu_device_owns(uint64_t number, uint64_t a1);
/**
 * @brief Dispatch a GPU ioctl after checking the descriptor's device identity.
 * @param number Linux syscall number.
 * @param a0 GPU descriptor.
 * @param a1 Ioctl request number.
 * @param a2 User request address, validated by the GPU handler.
 * @param a3 Unused syscall argument.
 * @param a4 Unused syscall argument.
 * @param a5 Unused syscall argument.
 * @return Device result or negative errno.
 */
int64_t syscall_gpu_device_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int syscall_device_owns(uint64_t number, uint64_t a1);
int64_t syscall_device_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int syscall_security_owns(uint64_t number, uint64_t a1);
int64_t syscall_security_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int proc_lookup(const char *path, struct storage_node *out);
int proc_read(const char *path, uint64_t offset, void *buffer, uint32_t length,
              uint32_t *out_read);
int proc_readlink(const char *path, char *buffer, uint32_t capacity);
/** @brief Acquire a proc FD magic-link target using READ_FSCREDS authorization.
 * @param target Stable target task under the execution transaction, nullable.
 * @param fd Target's descriptor number; resolves its current canonical OFD.
 * @param out Empty output receiving an independent mount/dentry reference.
 * @return Zero or negative EINVAL/ENOENT/EACCES. No pathname text is resolved;
 * the target may be an unlinked file on an unmounted anonymous backend.
 * Authorization precedes descriptor inspection. No graph lock spans callbacks.
 */
int proc_fd_get_path(struct task *target,int fd,struct vfs_path *out);
/**
 * @brief Enumerate generated proc entries using stable numeric process cookies.
 * @param path Borrowed intrinsic proc directory name.
 * @param offset Required in/out cookie, zero to rewind; process cookies outlive slot compaction.
 * @param entry Required writable record, assigned on success.
 * @return One entry, zero EOF, or negative errno. Caller serializes execution
 * and retains the provider; no scheduler lock spans a generated-file callback.
 */
int proc_readdir(const char *path, uint64_t *offset,
                 struct reliefos_dir_entry *entry);
/** @brief Dispatch borrowed raw arguments through the ordinary Linux syscall backends.
 * @param number Linux x86-64 syscall number.
 * @param a0 First raw argument; backend validates any user pointer before access.
 * @param a1 Second raw argument. @param a2 Third raw argument.
 * @param a3 Fourth raw argument. @param a4 Fifth raw argument.
 * @param a5 Sixth raw argument.
 * @return Backend result, negative errno or entry-owned blocking/retry status.
 * Execution-owned task context; no task/file/provider pin escapes this call.
 */
int64_t syscall_dispatch_regs_legacy(uint64_t number, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5);
int task_can_allocate_fd(const struct task *task);
struct task_pty_fd *task_pty_fd_for_fd(struct task *task, int fd);
void task_pty_release_entry(struct task_pty_fd *entry);
int task_pty_export_fd(struct task *task, int fd, struct task_pty_fd *out);
int task_pty_import_fd(struct task *task, const struct task_pty_fd *source, uint32_t flags);
void clear_task_file(struct task_file *file);
int64_t syscall_record_lock(int fd, uint32_t command, uint64_t pointer);
void syscall_record_locks_close(struct task *task, struct task_file *descriptor);
void syscall_record_lock_cancel(struct task *task);

/** @brief Execute a native realtime clock discipline request. */
int64_t syscall_adjtimex(int32_t clock, uint64_t address);

/** @brief Check CAP_SYS_MODULE and execute a native driver control request. */
int64_t syscall_driver_control(uint32_t request, uint64_t address);

/** @brief Execute authorized module operations with copied userspace inputs.
 * @param number Native Linux syscall number: init/delete/finit_module.
 * @param a0 Image/name user address or signed descriptor in the low 32 bits.
 * @param a1 Image length, delete flags or parameters user address.
 * @param a2 Parameters user address or finit flags.
 * @return Linux result/negative errno; execution transaction held by caller.
 */
int64_t syscall_modules(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2);

/** @brief Check finit_module authority/flags/OFD before allocating its I/O pin.
 * @param fd Linux descriptor number; only its signed low 32 bits are consumed.
 * @param flags Linux finit flags in the low 32 bits.
 * @return Zero or Linux errno, including ENOSYS before task lookup when disabled.
 * Task context under execution ownership; no usercopy, I/O or ref transfer.
 */
int syscall_module_file_prepare(uint64_t fd,uint64_t flags);

/** @brief Retire an ordinary vector when entry registers select a new call.
 * @param task Borrowed execution-owned caller, nullable.
 * @param frame Borrowed syscall frame, nullable; sigreturn preserves state.
 * @return None. Mismatches invoke common queue/drain/OFD cleanup before new
 * descriptor admission. No graph/module lock spans final provider release.
 */
void task_vector_enter(struct task *task,const struct trap_frame *frame);
/** @brief Release one task-owned imported iovec snapshot after I/O quiescence.
 * @param task Execution-owned owner, nullable; stored pointer clears first.
 * @return None. Caller has unlinked queued waits and drained owned storage.
 * This releases metadata only, with no OFD transfer or provider callback.
 */
void task_vector_cancel(struct task *task);
/** @brief Snapshot accepted vector progress before signal cancellation.
 * @param task Borrowed execution-owned owner, nullable.
 * @return Prior completed elements plus current accepted prefix, or zero
 * without a pending vector. No allocation, I/O or ownership change.
 */
uint64_t task_vector_progress(const struct task *task);

#endif

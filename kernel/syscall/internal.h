#ifndef RELIEFNT_KERNEL_SYSCALL_DISPATCH_C_INTERNAL_H
#define RELIEFNT_KERNEL_SYSCALL_DISPATCH_C_INTERNAL_H

#include "legacy.h"
/*
 * ReliefOS kernel system calls: validates and dispatches the user ABI.
 * Implements process, file, memory, IPC, GUI, device, and timing operations.
 */
#include <reliefnt/console.h>
#include <reliefnt/arch.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/drm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/lock.h>
#include <reliefnt/audio.h>
#include <reliefnt/heap.h>
#include <reliefnt/input.h>
#include <reliefnt/mm.h>
#include <reliefnt/mouse.h>
#include <reliefnt/net.h>
#include <reliefnt/permissions.h>
#include <reliefnt/platform.h>
#include <reliefnt/power.h>
#include <reliefnt/pty.h>
#include <reliefnt/random.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/userland.h>
#include <reliefnt/elf.h>
#include <reliefnt/kernel_debug.h>
#include <reliefnt/wait.h>
#include <reliefos/layout.h>


#ifndef RELIEFOS_SYSCALL_FRAME_DISPATCH
#define RELIEFOS_SYSCALL_FRAME_DISPATCH syscall_dispatch_regs
#endif


#include <reliefnt/version.h>

#include <reliefos/device_abi.h>
#include <reliefos/audio_abi.h>
#include <reliefos/driver.h>
#include <reliefos/auth_abi.h>
#include <reliefos/fs_abi.h>
#include <reliefos/fb.h>
#include <reliefos/net_abi.h>
#include <reliefos/pty_abi.h>
#include <reliefos/signal_abi.h>
#include <reliefos/system_abi.h>
#include <reliefos/startup_abi.h>
#include <reliefos/text.h>
#include <linux/tty.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <linux/soundcard.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/time.h>
#include <linux/resource.h>
#include <linux/sysinfo.h>
#include <linux/times.h>
#include <linux/uio.h>

#define RELIEFOS_TEXT_LAYOUT_MAX_BYTES 4096U
#define RELIEFOS_TEXT_LAYOUT_MAX_GLYPHS 512U
#define PAGE_SIZE 4096ULL
#define LINUX_PROT_READ TASK_VMA_PROT_READ
#define LINUX_PROT_WRITE TASK_VMA_PROT_WRITE
#define LINUX_PROT_EXEC TASK_VMA_PROT_EXEC
#define LINUX_MAP_PRIVATE 0x02u
#define LINUX_MAP_FIXED 0x10u
#define LINUX_MAP_ANONYMOUS 0x20u
#define LINUX_MAP_SUPPORTED (LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS)
#define SYSCONFDIALOG_APP_PATH RELIEFOS_LAYOUT_RELIEFOS_APPS "/sysconfdialog/sysconfdialog.elf"
#define STARTUP_DB_PATH RELIEFOS_PATH_STARTUP_DB
#define STARTUP_DENIAL_DB_PATH RELIEFOS_PATH_STARTUP_DENIALS_DB
#define STARTUP_DB_MAGIC 0x53545031U
#define STARTUP_DENIAL_DB_MAGIC 0x53544431U
#define STARTUP_DB_ENTRY_MAX 64U
#define STARTUP_DENIAL_MAX 64U
#define STARTUP_REQUEST_MAX 16U
#define TASK_EPOLL_MAX_ENTRIES 128u
#define LINUX_RWF_HIPRI 0x00000001u

struct task_timerfd {
    int32_t clockid;
    uint32_t flags;
    uint64_t expiry_tick;
    uint64_t interval_ticks;
    uint64_t expirations;
};

struct task_epoll_entry {
    int32_t fd;
    uint32_t events;
    uint64_t data;
    uint32_t ready;
    uint64_t generation;
    uint32_t active;
};

struct task_epoll {
    uint32_t entries;
    uint32_t reserved;
    struct task_epoll_entry item[TASK_EPOLL_MAX_ENTRIES];
};



#define RELIEFOS_FLOCK_SH 1u
#define RELIEFOS_FLOCK_EX 2u
#define RELIEFOS_FLOCK_NB 4u
#define RELIEFOS_FLOCK_UN 8u
#define RELIEFOS_FLOCK_MAX 64u

struct reliefos_flock_entry {
    uint32_t used;
    uint32_t type;
    struct task_file *owner;
    uint32_t shared_count;
    struct task_file *shared_owners[RELIEFOS_FLOCK_MAX];
    struct storage_node node;
    struct kernel_wait_queue waiters;
};



/* fbdev's colour map and blank state are software state on the VMware
 * framebuffer.  Keeping them here gives standard fb clients a stable
 * GETCMAP/PUTCMAP round trip even though SVGA scanout is true-colour. */



#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/dirent.h>
#include <linux/limits.h>
_Static_assert(sizeof(struct linux_stat_abi) == 144, "Linux x86-64 stat layout");












struct startup_db {
    uint32_t magic;
    uint32_t count;
    uint32_t next_id;
    uint32_t reserved;
    struct {
        uint32_t uid;
        struct reliefos_startup_entry entry;
    } entries[STARTUP_DB_ENTRY_MAX];
};

struct startup_denial_db {
    uint32_t magic;
    uint32_t count;
    uint32_t reserved0;
    uint32_t reserved1;
    struct {
        uint32_t uid;
        char requester_path[RELIEFOS_FS_PATH_LEN];
        struct reliefos_startup_command command;
    } entries[STARTUP_DENIAL_MAX];
};

struct startup_request_slot {
    uint32_t used;
    uint32_t id;
    uint32_t status;
    uint32_t requester_pid;
    uint32_t dialog_pid;
    uint32_t session_id;
    struct reliefos_user_info user;
    char requester_path[RELIEFOS_FS_PATH_LEN];
    struct reliefos_startup_command command;
};






struct task_snapshot_user {
    uint32_t capacity;
    uint32_t count;
    uint64_t tick;
    struct task_snapshot_info *tasks;
};







/**
 * Require window server.
 * @return The value or status produced by the operation.
 */










struct exec_params_kernel {
    uint32_t argc;
    uint32_t envc;
    char *argv[SCHED_EXEC_ARG_MAX + 1];
    char *envp[SCHED_EXEC_ENV_MAX + 1];
    char data[SCHED_EXEC_DATA_MAX];
    uint32_t data_len;
};
















































#define RELIEFOS_F_DUPFD LINUX_F_DUPFD
#define RELIEFOS_F_GETFD LINUX_F_GETFD
#define RELIEFOS_F_SETFD LINUX_F_SETFD
#define RELIEFOS_F_GETFL LINUX_F_GETFL
#define RELIEFOS_F_SETFL LINUX_F_SETFL
#define RELIEFOS_F_DUPFD_CLOEXEC LINUX_F_DUPFD_CLOEXEC




































































































































/*
 * Linux sendfile(2) transfers bytes from a regular input file without
 * changing its open-file-description offset when an explicit offset pointer
 * is supplied. Keep the copy in kernel memory so the output may be a pipe or
 * socket as well as another regular file.
 */


/* Linux copy_file_range has independent optional offsets for both files.
 * The storage backends are synchronous, so each completed chunk is visible
 * before the syscall advances either offset. */






























































/* Linux v6.12 fs/binfmt_script.c: a 256-byte header, one optional argument,
 * and no execution of a truncated interpreter pathname. */


































#define LINUX_SELECT_MAX_FDS 1024U











/* Translate the Linux fd_set ABI to the kernel's poll implementation. The
 * scheduler already owns the blocking/retry path; select only needs the
 * fixed-width bitsets and native timeout conversion around it. */


/* ppoll shares poll's descriptor ABI but carries a nanosecond timeout.  The
 * scheduler currently has no atomic signal-mask handoff for a parked poll;
 * reject a non-null mask instead of silently widening the interrupt window. */






/**
 * @brief Handles process identity, signal, and nice-style priority syscalls.
 * @param number Linux syscall number.
 * @param a0 First syscall argument.
 * @param a1 Second syscall argument.
 * @param a2 Third syscall argument.
 * @param a3 Fourth syscall argument.
 * @return Syscall result or negative errno.
 */


extern struct reliefos_flock_entry reliefos_flocks[RELIEFOS_FLOCK_MAX];

/**
 * @brief Remove a descriptor from every epoll set owned by a task.
 * @param task Descriptor-table owner.
 * @param fd Descriptor being closed or replaced.
 *
 * Linux removes an epoll registration when the last descriptor reference is
 * closed. Keeping a numeric fd here would otherwise let a later open reuse
 * the number while dispatching the old callback pointer, which is fatal for
 * Xorg's input thread during VT return.
 */
void task_epoll_remove_fd(struct task *task, int fd);

uint64_t linux_stat_inode(const char *path, const struct reliefos_stat *st);

int linux_stat_from_legacy(struct linux_stat_abi *out,
                                   const struct reliefos_stat *st,
                                   const char *path, const struct storage_node *node);

void linux_stat_fd_type(struct linux_stat_abi *out, const struct task_file *file);

void linux_statx_from_legacy(struct linux_statx *out,
                                    const struct linux_stat_abi *st,
                                    uint32_t mask);

/**
 * Path append char.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param ch Value supplied by the caller.
 */
void path_append_char(char *buf, uint32_t *pos, uint32_t cap, char ch);

/**
 * Text eq cstr.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int text_eq_cstr(const char *a, const char *b);

/**
 * Task effective role.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t task_effective_role(const struct task *task);

/**
 * Path append text.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param text NUL-terminated text supplied by the caller.
 */
void path_append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text);

/**
 * Path append u64.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param value Value supplied by the caller.
 */
void path_append_u64(char *buf, uint32_t *pos, uint32_t cap, uint64_t value);

/**
 * Copy text.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param src Value supplied by the caller.
 */
void copy_text(char *dst, uint32_t cap, const char *src);

void syscall_flock_release_owner(struct task_file *owner);

int64_t syscall_flock(uint64_t fd_arg, uint32_t operation);

/**
 * Clear task file.
 * @param file Value supplied by the caller.
 */
int clear_task_file_result(struct task_file *file);

void clear_task_file(struct task_file *file);

int task_file_reference(struct task_file *destination, struct task_file *source);

struct task_file *task_file_get(struct task_file *source);

void task_file_put(struct task_file *description);

/**
 * Syscall release task files.
 * @param task Value supplied by the caller.
 */
void syscall_release_task_files(struct task *task);

/**
 * @brief Retains pipe endpoint references after fork copies descriptor entries by value.
 * @param parent Source process, required to prevent accidental use on detached task records.
 * @param child Fork child whose copied pipe endpoints require new references.
 * @return Zero on success or a negative errno-style result for invalid arguments.
 */
int syscall_clone_task_files(const struct task *parent, struct task *child);

int syscall_unshare_task_files(struct task *task);

/**
 * @brief Closes file and explicit PTY aliases marked FD_CLOEXEC for execve.
 * @param task Process whose existing descriptor table survives the image replacement.
 */
void syscall_close_cloexec_files(struct task *task);

struct task_file *task_file_for_fd(struct task *task, int fd);

struct task_file *task_file_for_io(struct task *task, int fd);

/**
 * @brief Drop the private endpoint of an interrupted FIFO open.
 * @param task Task under the kernel execution lock; NULL is accepted.
 */
void task_fifo_cancel(struct task *task);

void task_release_syscall_file(struct task *task);

/**
 * Task pty fd for fd.
 * @param task Value supplied by the caller.
 * @param fd Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct task_pty_fd *task_pty_fd_for_fd(struct task *task, int fd);

/**
 * @brief Read Linux descriptor flags (FD_CLOEXEC) for any open descriptor.
 *
 * Linux keeps FD_CLOEXEC in the per-descriptor table entry, not in the shared
 * open file description, so a dup()ed descriptor never observes the original's
 * flag.  ReliefOS splits descriptors across three backing tables: file entries,
 * explicit PTY endpoints and implicit stdin/stdout/stderr streams.  Implicit
 * streams have no task_file entry and store the flag in cloexec_stdio_mask.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock so the tables cannot change concurrently.
 * @param fd Descriptor number; only the caller's low 32 bits are meaningful.
 * @return Non-negative flag bitmask (only RELIEFOS_FD_CLOEXEC is defined),
 *         or -RELIEFOS_EBADF when the descriptor is not open.
 */
int task_fd_descriptor_flags(struct task *task, int fd);

/**
 * @brief Update Linux descriptor flags (FD_CLOEXEC) on exactly one descriptor.
 *
 * The flag is stored per descriptor, so only the addressed slot changes and
 * dup()ed descriptors that share the same open file description keep their own
 * state.  Implicit stdin/stdout/stderr descriptors record the flag in
 * cloexec_stdio_mask so a later execve() really closes them instead of
 * reporting success without saving anything.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock so the tables cannot change concurrently.
 * @param fd Descriptor number; only the caller's low 32 bits are meaningful.
 * @param flags Replacement flag bitmask; every bit outside RELIEFOS_FD_CLOEXEC
 *              is ignored, matching Linux F_SETFD/do_vfs_ioctl().
 * @return Zero on success, or -RELIEFOS_EBADF when the descriptor is not open.
 */
int task_fd_set_descriptor_flags(struct task *task, int fd, uint32_t flags);

struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd);

struct task_pty_fd *task_pty_for_io(struct task *task, int fd);

uint32_t task_pty_status(const struct task_pty_fd *entry);

int task_pty_promote(struct task_pty_fd *entry);

void task_pty_release_entry(struct task_pty_fd *entry);

int task_pty_endpoint_fd(struct task *task, uint32_t pty_id,
                                uint32_t endpoint, uint32_t flags);

int task_pty_endpoint_path(const char *path, uint32_t *pty_id);

int64_t syscall_eventfd_create(uint64_t initial, uint32_t flags);

void task_timerfd_update(struct task_timerfd *timer);

int64_t syscall_timerfd_create(uint64_t clockid, uint32_t flags);

int64_t syscall_timerfd_settime(uint64_t fd_arg, uint32_t flags,
                                       uint64_t value_ptr, uint64_t old_ptr);

int64_t syscall_timerfd_gettime(uint64_t fd_arg, uint64_t value_ptr);

int64_t syscall_memfd_create(uint64_t name_ptr, uint32_t flags);

/**
 * Task pty stream for fd.
 * @param task Value supplied by the caller.
 * @param fd Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_pty_stream_for_fd(struct task *task, int fd);

int task_pty_ensure_fd(struct task *task, int fd, struct task_pty_fd **out);

/**
 * @brief Give implicit boot streams one shared, read/write /dev/console description.
 * @param task Descriptor owner; null and existing PTY streams are left alone.
 * @return Zero on success, negative ENODEV before VT setup, ENOMEM on allocation
 * failure, or the terminal reference error without publishing any descriptors.
 * Existing redirections and explicitly closed descriptors are preserved. Opening
 * /dev/console does not acquire a controlling terminal, matching Linux tty_open.
 */
int task_console_materialize_stdio(struct task *task);

/**
 * @brief Materialize inherited console or PTY streams before descriptor sharing.
 * @param task Descriptor owner; null is accepted.
 * @return Zero on success or a negative allocation/terminal-reference errno.
 */
int task_pty_materialize_stdio(struct task *task);

/**
 * Task pty fd available.
 * @param task Value supplied by the caller.
 * @param fd Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_pty_node_for_fd(struct task *task, int fd, struct storage_node *node,
                                 const char **path);

int task_pty_fd_available(struct task *task, int fd);

/**
 * @brief Find the lowest free descriptor below RLIMIT_NOFILE across all backing tables.
 * @param task Descriptor-table owner.
 * @param minimum Inclusive lower bound.
 * @return Descriptor number or negative errno.
 */
int task_unused_fd(struct task *task, int minimum);

int task_can_allocate_fd(const struct task *task);

/**
 * @brief Reserve the lowest free file descriptor, growing backing storage if needed.
 * @param task Descriptor-table owner, serialized by the syscall execution lock.
 * @param minimum Inclusive lower bound.
 * @param slot Receives the initialized descriptor entry.
 * @return Descriptor number or negative errno without consuming a slot on failure.
 */
int task_allocate_fd(struct task *task, int minimum, struct task_file **slot);

/**
 * @brief Release a file descriptor after a failed multi-fd operation, including stdio slots.
 * @param task Descriptor-table owner.
 * @param fd Reserved file descriptor to roll back.
 */
void task_discard_file_fd(struct task *task, int fd);

/**
 * Task pty duplicate fd.
 * @param task Value supplied by the caller.
 * @param old_fd Value supplied by the caller.
 * @param minimum_fd Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
int task_pty_duplicate_fd(struct task *task, int old_fd, int minimum_fd,
                                 uint32_t flags);

int task_pty_export_fd(struct task *task, int fd, struct task_pty_fd *out);

int task_pty_import_fd(struct task *task, const struct task_pty_fd *source, uint32_t flags);

/**
 * Task pty dup2 fd.
 * @param task Value supplied by the caller.
 * @param old_fd Value supplied by the caller.
 * @param new_fd Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_pty_dup2_fd(struct task *task, int old_fd, int new_fd);

/**
 * @brief Allocate an opened descriptor and retain its inode or console terminal.
 * @param task Descriptor-table owner under the execution transaction.
 * @param node Validated backing node, borrowed during initialization.
 * @param flags Validated status and internal device flags.
 * @param path Optional NUL-terminated device or file pathname.
 * @return Descriptor number or negative errno; failed console setup releases
 * the allocated slot and inode reference. O_PATH does not open a terminal.
 */
int alloc_task_fd(struct task *task, const struct storage_node *node, uint32_t flags, const char *path);

int task_dup2_fd(struct task *task, int old_fd, int new_fd);

int task_duplicate_file_fd(struct task *task, int old_fd, int minimum_fd,
                                  uint32_t fd_flags);

int syscall_inherit_task_fds(struct task *parent, struct task *child,
                             int stdin_fd, int stdout_fd, int stderr_fd);

/**
 * File can read.
 * @param file Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int file_can_read(const struct task_file *file);

/**
 * File can write.
 * @param file Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int file_can_write(const struct task_file *file);

/**
 * @brief Read one bounded slice of DRM event data into user memory.
 * @param task Current task used for the DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param buffer User address of the writable destination range.
 * @param length Requested writable byte range; one DRM read is capped at
 *        RELIEFOS_FS_IO_SLICE_BYTES.
 * @return Bytes read, or negative EBADF/EFAULT or the DRM read's errno.
 */
int64_t syscall_drm_read(struct task *task, struct task_file *file,
                                uint64_t buffer, uint64_t length);

/**
 * @brief Read DRM event data with a positional syscall's offset validation.
 * @param task Current task used for the DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param buffer User address of the writable destination range.
 * @param length Requested byte count, subject to the shared read slice limit.
 * @param position Signed file offset encoded in the syscall register; DRM event
 *        reads ignore its value after rejecting negative offsets.
 * @return Bytes read, negative EINVAL for a negative offset, or the DRM read's
 *         negative errno.
 */
int64_t syscall_drm_pread(struct task *task, struct task_file *file,
                                 uint64_t buffer, uint64_t length,
                                 uint64_t position);

/**
 * @brief Apply DRM's no-op seek behavior after validating the seek request.
 * @param file Borrowed task file; it must refer to a DRM file description.
 * @param whence Linux seek selector, checked by the existing 32-bit <= 4 rule.
 * @return The unchanged file offset, or negative EBADF/EINVAL on invalid input.
 */
int64_t syscall_drm_llseek(struct task_file *file, uint64_t whence);

/**
 * @brief Read DRM event data across an already imported iovec array.
 * @param task Current task used for DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param iov Kernel-side iovec copy imported by the vector syscall dispatcher;
 *        each writable user range is checked before any read is issued.
 * @param iovcnt Number of entries in iov; values above UIO_MAXIOV are rejected.
 * @param positional Whether position is a preadv offset that needs validation.
 * @param position Signed file offset encoded in the syscall register; DRM event
 *        reads ignore it after rejecting a negative positional offset.
 * @param flags Vector-read flags; zero and LINUX_RWF_HIPRI are supported.
 * @return Total bytes read, a partial count after a later read error, or
 *         negative EBADF/EINVAL/EFAULT/EOPNOTSUPP or the DRM read's errno.
 */
int64_t syscall_drm_readv(struct task *task, struct task_file *file,
                                 const struct iovec *iov, uint64_t iovcnt,
                                 bool positional, uint64_t position,
                                 uint64_t flags);

int task_device_is(const struct task_file *file, uint32_t kind);

/**
 * @brief Resolve the VT that owns an evdev-opening task.
 * @param task Current task; may be null.
 * @return Fixed VT number, or zero for an unbound raw evdev client.
 *
 * Xorg opens its input devices before it switches the VT to KD_GRAPHICS.  The
 * active-graphics bit is therefore not a reliable open-time hint.  Its
 * inherited tty endpoint is stable across that transition, so prefer the
 * controlling tty and then any explicit VT endpoint in the descriptor table.
 */
uint32_t task_evdev_vt_hint(const struct task *task);

/**
 * @brief Bind an evdev descriptor to a VT once its owner opens that VT.
 * @param task Current task; may be null for an invalid syscall context.
 * @param file Evdev open-file description to update.
 * @return Nothing; an already-bound or otherwise unresolvable descriptor is unchanged.
 *
 * Xorg can open /dev/input/event* before xf86OpenConsole creates its explicit
 * /dev/ttyN endpoint.  The open-time hint is then zero and would leave the
 * descriptor on the raw stream forever.  Retry the same stable descriptor
 * lookup at the first read/poll after the tty endpoint exists.  Keep the
 * existing cursor so VT hand-off snapshots that were queued in between are
 * still delivered to the newly bound consumer.
 */
void task_evdev_bind_vt(struct task *task, struct task_file *file);

/**
 * @brief Decode a kernel-created descriptor backed by a genuine block device node.
 * @param file Descriptor to validate; may be null.
 * @param disk_id Optional physical disk ID output.
 * @param partition_index Optional GPT entry index output, or -1 for the whole disk.
 * @return One for a block descriptor, zero for all other descriptors.
 */
int task_block_device(const struct task_file *file, uint32_t *disk_id,
                             int32_t *partition_index);

/**
 * @brief Read an opened device; block descriptors use Linux open-time DAC checks.
 * @param task Current task, required for device context.
 * @param file Opened readable device descriptor.
 * @param buffer Validated writable destination of length bytes.
 * @param length Maximum byte count to read.
 * @return Bytes read, negative errno, or KERNEL_SYSCALL_BLOCKED for terminal
 * job control; an empty nonblocking console returns negative EAGAIN.
 */
int task_device_read(struct task *task, struct task_file *file,
                            void *buffer, uint32_t length);

/** @brief Bound one device write before narrowing its 64-bit user length.
 * @param file Borrowed description under the execution transaction.
 * @param count Validated user byte count.
 * @return Request bytes; no ownership, allocation, wait or IRQ change occurs.
 * Task context. PCM adapters retain their small internal copy buffers, queue
 * limits, short transfers and interruptible waits. Other devices use the
 * filesystem slice. A 32 KiB PCM request avoids forcing one Doom mixed block
 * through five syscall returns while bounding the execution transaction.
 */
uint32_t task_device_write_length(const struct task_file *file, uint64_t count);

/**
 * @brief Write a device, applying terminal processing to system-console output.
 * @param task Calling task, used for permissions and terminal job control.
 * @param file Writable opened device description; console aux retains its VT.
 * @param buffer Validated source of length bytes.
 * @param length Bounded device transfer size in bytes.
 * @return Accepted bytes, negative errno, or KERNEL_SYSCALL_BLOCKED for job
 * control. Accepted console bytes are also mirrored to diagnostic serial.
 */
int task_device_write(struct task *task, struct task_file *file,
                             const void *buffer, uint32_t length);

/**
 * @brief Write global VT query results after a VT descriptor has been resolved.
 * @param request VT_OPENQRY or VT_GETSTATE.
 * @param user_arg User output address, checked for writable access here.
 * @return Zero on success or a negative errno.
 */
int task_vt_query_ioctl(uint64_t request, uint64_t user_arg);

int task_evdev_ioctl(struct task_file *file, uint64_t request,
                            uint64_t user_arg);

/**
 * Copy user path.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param user_ptr Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_user_path(char *dst, uint32_t cap, uint64_t user_ptr);

/**
 * Resolve user path.
 * @param task Value supplied by the caller.
 * @param user_ptr Value supplied by the caller.
 * @param out Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int resolve_user_path_ids(struct task *task, uint64_t user_ptr, char *out,
                                  uint32_t cap, bool real_ids);

int resolve_user_path(struct task *task, uint64_t user_ptr, char *out, uint32_t cap);

int resolve_kernel_path_at_flags(struct task *task, int32_t dirfd, const char *raw,
                                bool allow_empty, char *path, struct task_file **empty_file,
                                bool real_ids, uint32_t lookup_flags);

/** @brief Resolves a raw *at pathname with an explicit final-symlink policy. */
int resolve_user_path_at_flags(struct task *task, int32_t dirfd, uint64_t user_ptr,
                                bool allow_empty, char *path, struct task_file **empty_file,
                                bool real_ids, uint32_t lookup_flags);

/**
 * @brief Validates and strips a trailing slash on a parent-resolved mutation path.
 * @param path Mutable resolved pathname.
 * @param creating Nonzero for non-directory creation, which rejects existing names.
 * @return Zero or Linux's trailing slash error without following the last symlink.
 */
int mutation_path_slash(char *path, bool creating);

/**
 * @brief Classifies final dot components before any storage lexical normalization.
 * @param path Absolute parent-resolved path, possibly with a trailing slash.
 * @return 1 for dot, 2 for dot-dot, 3 for root, otherwise 0.
 */
int mutation_path_special(const char *path);

/**
 * Storage errno.
 * @param ret Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int storage_errno(int ret);

int64_t syscall_sendfile(uint64_t out_fd_arg, uint64_t in_fd_arg,
                                uint64_t offset_arg, uint64_t count_arg);

int64_t syscall_copy_file_range(uint64_t in_fd_arg, uint64_t off_in_arg,
                                       uint64_t out_fd_arg, uint64_t off_out_arg,
                                       uint64_t len_arg, uint64_t flags_arg);

/**
 * Copy user string fixed.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param user_ptr Value supplied by the caller.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_user_string_fixed(char *dst, uint32_t cap, uint64_t user_ptr, uint32_t *out_len);

/**
 * Kernel string len cap.
 * @param text NUL-terminated text supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t kernel_string_len_cap(const char *text, uint32_t cap);

/**
 * Copy exec params from user.
 * @param argv_ptr Value supplied by the caller.
 * @param envp_ptr Value supplied by the caller.
 * @param params Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_exec_params_from_user(uint64_t argv_ptr, uint64_t envp_ptr, struct exec_params_kernel *params);

int exec_resolve_scripts(struct task *task, char *path, struct storage_node *node,
                                 const char *execfn, bool inaccessible,
                                 struct exec_params_kernel *params);

/**
 * Stat for fd.
 * @param fd Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @param st Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int stat_for_fd(int fd, struct task *task, struct reliefos_stat *st);

/**
 * @brief Park the caller until readiness, the one-tick backstop, or timeout.
 *
 * Pollable sources publish a generation change and wake this shared queue.
 * The generation comparison is atomic with queue registration, closing the
 * check-to-sleep race. The tick deadline remains a conservative backstop for
 * readiness sources that do not publish notifications.
 */
int64_t syscall_poll_park(struct task *task, int64_t timeout_ms,
                                 uint64_t generation, bool can_wait_for_readiness);

/**
 * @brief Poll descriptors using their bound terminal or device readiness.
 * @param task Calling task under the execution transaction.
 * @param fds Validated in/out poll array; nullable when nfds is zero.
 * @param nfds Number of entries.
 * @param timeout_ms Linux poll timeout, narrowed to signed 32 bits.
 * @param generation Readiness generation captured before scanning descriptors.
 * @return Ready count, zero on timeout, negative errno or a blocked retry token.
 */
int64_t syscall_poll_impl(struct task *task, struct pollfd *fds,
                                 uint32_t nfds, int64_t timeout_ms,
                                 uint64_t generation);

int64_t syscall_poll(uint64_t fds_ptr, uint64_t count, int64_t timeout_ms);

int64_t syscall_epoll_create(uint64_t size, uint32_t flags);

int64_t syscall_epoll_ctl(uint64_t epfd_arg, uint64_t op_arg,
                                 uint64_t fd_arg, uint64_t event_ptr);

int64_t syscall_epoll_wait(uint64_t epfd_arg, uint64_t events_ptr,
                                  uint64_t maxevents_arg, int64_t timeout_ms);

int64_t syscall_epoll_pwait2(uint64_t epfd, uint64_t events, uint64_t maxevents,
                                    uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize);

int64_t syscall_getrusage(uint64_t who_arg, uint64_t usage_ptr);

int64_t syscall_sysinfo(uint64_t info_ptr);

int64_t syscall_times(uint64_t tms_ptr);

int64_t syscall_select_common(uint64_t nfds_arg, uint64_t read_ptr,
                                     uint64_t write_ptr, uint64_t except_ptr,
                                     uint64_t timeout_ptr, bool pselect_timeout);

int64_t syscall_ppoll(uint64_t fds_ptr, uint64_t count, uint64_t timeout_ptr,
                             uint64_t signal_mask_ptr, uint64_t signal_mask_size);

/**
 * @brief Implements the validated Linux openat2 argument block and delegates
 * the supported no-resolve path to the existing openat object allocator.
 * @param dirfd Directory descriptor used for relative resolution.
 * @param pathname User pathname pointer.
 * @param how_ptr User open_how pointer.
 * @param size Size of the user open_how block.
 * @return New descriptor or negative errno.
 */
int64_t syscall_openat2(uint64_t dirfd, uint64_t pathname,
                               uint64_t how_ptr, uint64_t size);

void syscall_init(void);

/**
 * Syscall dispatch.
 * @param frame Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int64_t syscall_dispatch(const struct syscall_frame *frame);

/**
 * Syscall dispatch regs legacy.
 * @param number Value supplied by the caller.
 * @param a0 Value supplied by the caller.
 * @param a1 Value supplied by the caller.
 * @param a2 Value supplied by the caller.
 * @param a3 Value supplied by the caller.
 * @param a4 Value supplied by the caller.
 * @param a5 Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int64_t syscall_update_path_times(struct task *task, int32_t dirfd, uint64_t path_ptr,
                                         const struct linux_timespec times[2], uint32_t flags,
                                         bool times_supplied);

/**
 * @brief Copy, validate, and copy back one fbdev variable-screen request.
 * @param user_info Readable user address of a complete fb_var_screeninfo.
 * @return Zero on success, or a negative Linux errno from validation/copyout.
 */
int64_t syscall_fbdev_put_var(uint64_t user_info);

/**
 * @brief Run the shared write backend without unrelated legacy syscall locals.
 * @param a0 Descriptor number; entry may already pin its open description.
 * @param a1 User buffer, validated by the backend before accessing its bytes.
 * @param a2 Requested byte count, subject to each backend's existing limits.
 * @return Existing byte count, errno, or internal blocking/retry result.
 */
int64_t syscall_io_write(uint64_t a0, uint64_t a1, uint64_t a2);

/**
 * @brief Run the shared read backend without unrelated legacy syscall locals.
 * @param a0 Descriptor number; entry may already pin its open description.
 * @param a1 User buffer, validated by the backend before accessing its bytes.
 * @param a2 Requested byte count, subject to each backend's existing limits.
 * @return Existing byte count, errno, or internal blocking/retry result.
 */
int64_t syscall_io_read(uint64_t a0, uint64_t a1, uint64_t a2);

int64_t syscall_dispatch_regs_legacy(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/** @brief Parse the optional boot syscall-trace=/path/prefix diagnostic filter. */
void syscall_trace_configure(const char *cmdline);

struct task *syscall_dispatch_frame(struct trap_frame *frame);

/**
 * @brief Dispatch normalised registers under the existing execution transaction.
 * @param task Current task; any open-description pin remains owned by the entry.
 * @param number Native syscall number.
 * @param a0 First ABI argument.
 * @param a1 Second ABI argument.
 * @param a2 Third ABI argument.
 * @param a3 Fourth ABI argument.
 * @param a4 Fifth ABI argument.
 * @param a5 Sixth ABI argument.
 * @return Original result, negative errno, or internal blocking status.
 */
int64_t syscall_dispatch_regs(struct task *task, uint64_t number, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#endif

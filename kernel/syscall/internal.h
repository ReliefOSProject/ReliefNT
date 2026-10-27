#ifndef RELIEFNT_KERNEL_SYSCALL_DISPATCH_C_INTERNAL_H
#define RELIEFNT_KERNEL_SYSCALL_DISPATCH_C_INTERNAL_H

#include "legacy.h"
#include "../exec/arguments.h"
/*
 * ReliefOS kernel system calls: validates and dispatches the user ABI.
 * Implements process, file, memory, IPC, GUI, device, and timing operations.
 */
#include <reliefnt/console.h>
#include <reliefnt/chrdev.h>
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
#define TASK_EPOLL_INITIAL_ENTRIES 16u

struct task_timerfd {
    int32_t clockid;
    uint32_t flags;
    uint64_t expiry_tick;
    uint64_t interval_ticks;
    uint64_t expirations;
};

/**
 * @brief One Linux epoll registration, keyed by descriptor and OFD identity.
 * @note An empty interest mask still observes unconditional POLLERR/POLLHUP.
 * The disabled bit represents EPOLLONESHOT delivery without destroying the
 * configured mask needed by a later EPOLL_CTL_MOD rearm.
 */
struct task_epoll_entry {
    int32_t fd;
    uint32_t events;
    uint64_t data;
    uint32_t ready;
    uint64_t generation;
    uint32_t active;
    uint32_t disabled;
    struct task_epoll_watch *watch;
};

/**
 * @brief Dynamically sized storage for an epoll OFD's active registrations.
 * @note The vector and each watch node are owned by this epoll; watched target
 * descriptions are weakly referenced by reverse lists and never kept alive.
 */
struct task_epoll {
    uint32_t entries;
    uint32_t capacity;
    struct task_epoll_entry *item;
};

/**
 * @brief Link one epoll registration to the watched OFD's final-release list.
 * @note The target pointer is borrowed. Descriptor, SCM_RIGHTS, and active I/O
 * references keep it alive; the final-release path detaches this watch before
 * freeing the target. This node itself does not own either the target or epoll.
 */
struct task_epoll_watch {
    struct task_epoll_watch *next;
    struct task_epoll *epoll;
    struct task_file *file;
    struct task_pty_description *pty;
    struct task_pty_fd pty_alias;
    uint32_t slot;
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
 * @brief Remove all epoll registrations borrowing a final-release file OFD.
 * @param description Stable canonical file description with zero references;
 *        the caller invokes this before releasing its resources or freeing it.
 * @note The kernel execution transaction serializes this callback with epoll
 * probes, and each linked watch is unlinked and freed before this returns.
 */
void task_epoll_remove_file_description(struct task_file *description);

/**
 * @brief Remove all epoll registrations borrowing a final-release PTY OFD.
 * @param description Stable PTY description whose total references reached zero.
 * @note The caller invokes this before dropping the terminal transfer reference
 * or freeing the description; probes and final release share the execution lock.
 */
void task_epoll_remove_pty_description(struct task_pty_description *description);

/**
 * @brief Release an epoll instance and its dynamically allocated watch storage.
 * @param epoll Heap-owned epoll state; null is ignored.
 */
void task_epoll_destroy(struct task_epoll *epoll);

/** @brief Encode admitted pathless node metadata without namespace replay.
 * @param out Required Linux stat output. @param st Optional builtin type/size.
 * @param node Optional already-held anonymous/backend/device node.
 * @return Zero or negative metadata errno. Caller keeps descriptor/inode/code
 * alive under execution ownership without graph/module callback locks. No
 * ownership transfer; mounted paths use linux_stat_from_path instead.
 */
int linux_stat_from_node(struct linux_stat_abi *out,const struct reliefos_stat *st,
                            const struct storage_node *node);

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
 * @brief Release a descriptor or canonical open-file description and its resources.
 * @param file Descriptor slot or stable OFD to clear; null and unused entries are ignored.
 * @return Backing inode release result, or zero when no inode release is needed.
 * @note A final canonical release detaches weak epoll watches before releasing
 * OFD-owned resources. SCM queue and in-flight I/O references use this same path.
 */
int clear_task_file_result(struct task_file *file);

/**
 * @brief Clear a descriptor slot and release its resources.
 * @param file Descriptor or canonical OFD slot; null is ignored.
 */
void clear_task_file(struct task_file *file);

/**
 * @brief Add a shared descriptor reference to a destination slot.
 * @param destination Unused slot to initialize; must not alias source.
 * @param source Live descriptor or canonical OFD.
 * @return Zero on success, -EBADF for invalid arguments, or -ENOMEM if promotion fails.
 */
int task_file_reference(struct task_file *destination, struct task_file *source);

/**
 * @brief Retain a transient reference to the source's stable canonical OFD.
 * @param source Live descriptor or description, serialized against close.
 * @return Canonical OFD with one additional reference, or null for invalid input
 * or promotion allocation failure.
 */
struct task_file *task_file_get(struct task_file *source);

/**
 * @brief Drop a transient canonical OFD reference and perform final cleanup.
 * @param description Canonical file description returned by task_file_get().
 * @note Final release detaches weak epoll registrations before freeing resources.
 */
void task_file_put(struct task_file *description);

/**
 * @brief Release a final task's descriptor entries and their backing vectors.
 * @param task Task whose file-table reference is already known to be final; NULL is accepted.
 */
void syscall_release_task_files(struct task *task);

/**
 * @brief Retain copied pipe and PTY endpoint references in a fork child's private table.
 * @param parent Source process, required to prevent accidental use on detached task records.
 * @param child Fork child whose copied endpoint entries require new references.
 * @return Zero on success or a negative errno-style allocation/reference error.
 * @note PTY alias slots are reserved before references are copied. If this fails
 * before the copy pass, no child PTY references are published; clone rollback owns
 * and releases any detached child vector.
 */
int syscall_clone_task_files(const struct task *parent, struct task *child);

/**
 * @brief Detach one task from a shared descriptor table while retaining each open reference.
 * @param task Task whose current descriptor table is shared; required.
 * @return Zero when already private or after a successful copy, otherwise a negative
 * allocation or terminal-reference error with the shared table still installed.
 */
int syscall_unshare_task_files(struct task *task);

/**
 * @brief Closes file and explicit PTY aliases marked FD_CLOEXEC for execve.
 * @param task Process whose existing descriptor table survives the image replacement.
 */
void syscall_close_cloexec_files(struct task *task);

struct task_file *task_file_for_fd(struct task *task, int fd);

/**
 * @brief Resolve the stable OFD bound to the current syscall or descriptor.
 * @param task Current task; NULL returns NULL.
 * @param fd Linux descriptor number whose in-flight syscall pin may override lookup.
 * @return Borrowed stable file description, or NULL when the descriptor is not a file.
 * @note Caller serializes with descriptor mutation; the returned pointer is not retained.
 */
struct task_file *task_file_for_io(struct task *task, int fd);

/**
 * @brief Drop the private endpoint of an interrupted FIFO open.
 * @param task Task under the kernel execution lock; NULL is accepted.
 */
void task_fifo_cancel(struct task *task);

/** @brief Cancel pending pathname, snapshot, OFD/PTY and subsystem ownership.
 * @param task Calling task under execution ownership, nullable.
 * Detaches ownership first; final backend callbacks may perform I/O, so no
 * scheduler, graph or module-table spinlock may span this cleanup.
 */
void task_release_syscall_file(struct task *task);

/**
 * @brief Find the PTY alias corresponding to a Linux descriptor.
 * @param task Descriptor-table owner; NULL is accepted.
 * @param fd Linux descriptor number.
 * @return Borrowed alias entry or NULL when no PTY alias matches.
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

/**
 * @brief Resolve a descriptor to a Unix98 master/slave endpoint alias.
 * @param task Descriptor-table owner; NULL is accepted.
 * @param fd Linux descriptor number.
 * @return Borrowed endpoint alias or NULL for non-endpoint aliases.
 */
struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd);

/**
 * @brief Resolve the PTY alias used by the current syscall's I/O path.
 * @param task Current syscall task; NULL is accepted.
 * @param fd Linux descriptor number.
 * @return Borrowed syscall pin or endpoint entry, otherwise NULL.
 */
struct task_pty_fd *task_pty_for_io(struct task *task, int fd);

/**
 * @brief Read status flags from a PTY open-file description or unpromoted alias.
 * @param entry PTY descriptor alias; caller supplies a live entry.
 * @return Shared Linux status flag word.
 */
uint32_t task_pty_status(const struct task_pty_fd *entry);

/**
 * @brief Promote one alias to a shared open-file description and retain its PTY.
 * @param entry Live alias whose descriptor reference is promoted in place.
 * @return Zero on success or a negative allocation/session-reference error.
 */
int task_pty_promote(struct task_pty_fd *entry);

/**
 * @brief Release one PTY alias and its description reference.
 * @param entry Alias entry; null and already released entries are ignored.
 * @note Final total release removes weak epoll watches before dropping terminal
 * ownership or freeing the description.
 */
void task_pty_release_entry(struct task_pty_fd *entry);

/**
 * @brief Allocate a master or slave PTY endpoint descriptor.
 * @param task Descriptor-table owner; required.
 * @param pty_id Live stable PTY session ID.
 * @param endpoint Master or slave endpoint kind.
 * @param flags Open status and close-on-exec flags.
 * @return New descriptor or negative errno; failure leaves no live alias.
 */
int task_pty_endpoint_fd(struct task *task, uint32_t pty_id,
                         uint32_t endpoint, uint32_t flags);

/**
 * @brief Parse a live numeric devpts path or resolve the active-VT alias.
 * @param path NUL-terminated endpoint path; NULL is rejected.
 * @param pty_id Writable result for the recognized active PTY identifier.
 * @return One for a recognized live endpoint path, zero for malformed, inactive,
 * or out-of-range paths.
 * @note Numeric PTY names are bounded by the signed Linux PTY identifier range.
 */
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

/**
 * @brief Materialize and promote an implicit PTY stream descriptor if needed.
 * @param task Descriptor-table owner; required.
 * @param fd Existing implicit stream or PTY alias descriptor.
 * @param out Receives a borrowed live alias entry; later PTY table growth can invalidate it.
 * @return Zero on success or negative errno without losing descriptor state.
 */
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

/** @brief Describe the admitted builtin terminal without namespace lookup.
 * @param task Borrowed current task under execution ownership.
 * @param fd Descriptor number with original PTY admission where applicable.
 * @param node Required caller-owned device identity result.
 * @param path Required output borrowing a static intrinsic metadata label.
 * @return Zero or negative errno; no allocation, I/O or reference transfer.
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
 * @brief Duplicate a PTY descriptor at or above a requested minimum.
 * @param task Descriptor-table owner; required.
 * @param old_fd Existing PTY or implicit stream descriptor.
 * @param minimum_fd Inclusive lowest allowed descriptor number.
 * @param flags Per-descriptor flags; FD_CLOEXEC is retained.
 * @return New descriptor or negative errno without altering source ownership.
 */
int task_pty_duplicate_fd(struct task *task, int old_fd, int minimum_fd,
                          uint32_t flags);

/**
 * @brief Retain a PTY alias for a Unix-domain SCM_RIGHTS transfer.
 * @param task Descriptor-table owner; required.
 * @param fd Open PTY or implicit PTY stream descriptor.
 * @param out Caller-owned transfer entry receiving one retained reference.
 * @return Zero on success or negative errno without changing output ownership.
 */
int task_pty_export_fd(struct task *task, int fd, struct task_pty_fd *out);

/**
 * @brief Import a retained PTY transfer entry into a task descriptor table.
 * @param task Receiving descriptor-table owner; required.
 * @param source Caller-owned live transfer entry; it remains owned by caller.
 * @param flags Per-descriptor flags to install on the received alias.
 * @return New descriptor or negative errno; source ownership is unchanged.
 * @note The source is snapshotted before possible table growth. If it points into
 * this same task's alias vector, growth may invalidate the caller's borrowed pointer.
 */
int task_pty_import_fd(struct task *task, const struct task_pty_fd *source, uint32_t flags);

/**
 * @brief Replace a target descriptor with a retained PTY alias.
 * @param task Descriptor-table owner; required.
 * @param old_fd Existing PTY or implicit stream descriptor.
 * @param new_fd Target descriptor, required to be below RLIMIT_NOFILE.
 * @return Target descriptor or negative errno without losing source ownership.
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



/**
 * @brief Replace a descriptor with a retained source description under the execution lock.
 * @param task Descriptor-table owner; required.
 * @param old_fd Existing source descriptor whose description remains shared.
 * @param new_fd Destination descriptor below RLIMIT_NOFILE.
 * @return Destination descriptor, or negative errno preserving the target on
 * failure. Replacement releases the old target and clears FD_CLOEXEC unless
 * the source and destination numbers are equal.
 */
int task_dup2_fd(struct task *task, int old_fd, int new_fd);

int task_duplicate_file_fd(struct task *task, int old_fd, int minimum_fd,
                                  uint32_t fd_flags);

/**
 * @brief Retain selected parent descriptors as a child's standard streams.
 * @param parent Source task; required when any requested descriptor is nonnegative.
 * @param child Destination task whose stdio aliases are replaced as needed.
 * @param stdin_fd Source descriptor for child fd 0, or a negative value to leave it alone.
 * @param stdout_fd Source descriptor for child fd 1, or a negative value to leave it alone.
 * @param stderr_fd Source descriptor for child fd 2, or a negative value to leave it alone.
 * @return Zero on success or a negative errno; retained PTY aliases are rolled back
 * if the destination table cannot grow.
 */
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

/** @brief Copy a bounded user pathname and resolve it to a retained object.
 * @param ns Shared live namespace under execution ownership.
 * @param task Borrowed current task with referenced filesystem state.
 * @param dirfd Linux descriptor or AT_FDCWD; absolute input ignores it.
 * @param user_ptr User address, checked including its terminating NUL.
 * @param allow_empty Allow the exact cwd/OFD object for empty input.
 * @param real_ids Select real rather than filesystem credentials for search.
 * @param lookup_flags VFS_LOOKUP_FOLLOW/PARENT/MISSING bits.
 * @param out Empty output receiving one owned path on success.
 * @return Zero or negative errno. Releases its temporary copy on every path;
 * an active syscall scope retains its own checked input and directory objects
 * across EAGAIN. User memory is never retained or reread by a matching retry.
 */
int resolve_user_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                   uint64_t user_ptr,bool allow_empty,bool real_ids,
                                   uint32_t lookup_flags,struct vfs_lookup_result *out);

/** @brief Derive authorized creation metadata from the actual parent inode.
 * @param task Borrowed current task and stable filesystem credentials.
 * @param parent Borrowed retained directory, never resolved by display name.
 * @param type Linux S_IF* inode type.
 * @param mode Requested permissions, subject to umask and set-ID policy.
 * @param out Required metadata output on success.
 * @return Zero or negative errno. Checks parent write/search, fsuid/fsgid,
 * SGID inheritance and capabilities; symlinks keep0777. No graph lock spans I/O.
 */
int syscall_creation_mode(struct task *task,const struct vfs_path *parent,uint32_t type,
                             uint32_t mode,struct reliefos_permissions *out);

/** @brief Create a directory/regular/special/link member through its held parent.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with retained root/cwd/dirfd state.
 * @param dirfd Relative base descriptor or AT_FDCWD.
 * @param pathname User pathname captured by the common retry scope.
 * @param type Supported Linux S_IF* inode type.
 * @param mode Requested permissions, with normal umask/inheritance semantics.
 * @param target Borrowed kernel-owned symlink target, NULL for other types.
 * @return Zero or negative errno. Existing names return EEXIST before parent
 * write checks; no fallible inode-open/VFS allocation follows publication.
 * No diagnostic-name replay or graph/module lock spans provider callbacks.
 */
int64_t syscall_create_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                uint64_t pathname,uint32_t type,uint32_t mode,const char *target);

/** @brief Check actual parent write/search and optional sticky target ownership.
 * @param task Borrowed caller with stable filesystem credentials.
 * @param parent Held containing directory, retained across provider callbacks.
 * @param member Held target, NULL for a new name without sticky checks.
 * @return Zero or negative errno. CAP_FOWNER bypasses only sticky ownership;
 * no graph/module lock spans metadata I/O and no pathname replay is performed.
 */
int syscall_mutation_parent(struct task *task,const struct vfs_path *parent,
                               const struct vfs_path *member);

/** @brief Unlink or remove a directory through the original retained parent.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with retained task/OFD paths and credentials.
 * @param dirfd Relative directory descriptor or AT_FDCWD.
 * @param pathname User operand copied once by the retry scope.
 * @param directory True for rmdir/AT_REMOVEDIR; false for unlink.
 * @return Zero or negative errno. Final symlinks are not followed; directory,
 * sticky, mountpoint and readonly checks precede publication. Open inode pins
 * survive removal. No scheduler/module/graph lock spans provider I/O.
 */
int64_t syscall_remove_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                 uint64_t pathname,bool directory);
/** @brief Rename entries using retained original source/destination parents.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable credentials and filesystem paths.
 * @param oldfd Source relative base descriptor or AT_FDCWD.
 * @param oldname Source user pathname copied once by the retry scope.
 * @param newfd Destination relative base descriptor or AT_FDCWD.
 * @param newname Destination user pathname, a separate retained operand.
 * @param flags renameat2 flags; unsupported nonzero atomics fail explicitly.
 * @return Zero or negative errno. Parent/sticky DAC and trailing/type checks
 * precede graph/provider mutation; final symlinks are not followed. Original
 * directories survive retries. No graph/module lock spans provider callbacks.
 */
int64_t syscall_rename_object(struct vfs_namespace *ns,struct task *task,int32_t oldfd,uint64_t oldname,
                                 int32_t newfd,uint64_t newname,uint32_t flags);
/** @brief Link an exact held source inode into the original destination parent.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable filesystem credentials and paths.
 * @param oldfd Relative source descriptor or AT_FDCWD.
 * @param oldname User source pathname copied once before retryable I/O.
 * @param newfd Relative destination directory descriptor or AT_FDCWD.
 * @param newname User destination pathname, copied as a separate operand.
 * @param flags AT_SYMLINK_FOLLOW and/or AT_EMPTY_PATH, no other bits.
 * @return Zero or negative errno. Empty-source linking follows the documented
 * CAP_DAC_READ_SEARCH gate; no directory or zero-link resurrection is allowed.
 * Caller retains both operands until common cancellation; no graph/module
 * lock, pathname-display authority or post-publication allocation occurs.
 */
int64_t syscall_link_object(struct vfs_namespace *ns,struct task *task,int32_t oldfd,uint64_t oldname,
                               int32_t newfd,uint64_t newname,uint32_t flags);

/** @brief Capture a symlink target once and create its actual directory member.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with optional active retry scope.
 * @param target User symlink payload, copied before any retryable filesystem I/O.
 * @param dirfd Relative link-directory descriptor or AT_FDCWD.
 * @param pathname User link pathname, captured independently by the resolver.
 * @return Zero or negative errno. The task owns active copies until common
 * cancellation; unscoped temporaries are freed. No locks span metadata I/O.
 */
int64_t syscall_symlink_object(struct vfs_namespace *ns,struct task *task,uint64_t target,
                                 int32_t dirfd,uint64_t pathname);

/** @brief Dispatch directory operations against retained task root/cwd objects.
 * @param ns Borrowed published namespace under execution ownership.
 * @param task Borrowed current task; shared CLONE_FS state is honored.
 * @param number Linux GETCWD/CHDIR/FCHDIR/CHROOT syscall number.
 * @param a0 User input/output pointer or FCHDIR directory descriptor.
 * @param a1 GETCWD buffer capacity; otherwise ignored.
 * @return Zero for directory changes, raw getcwd bytes including NUL or negative
 * errno. Errors preserve references; chroot preserves cwd. Backend search may
 * yield EAGAIN without a graph/scheduler lock; common syscall cleanup retires
 * the retained resolver state. Unknown operations return ENOSYS.
 */
int64_t syscall_directory(struct vfs_namespace *ns,struct task *task,uint64_t number,
                            uint64_t a0,uint64_t a1);

/** @brief Format the held inode's native stat metadata and device identity.
 * @param out Required Linux stat output, empty on metadata failure.
 * @param path Borrowed retained inode; no displayed name is consulted.
 * @return Zero or negative errno. Caller authorizes traversal and retains the
 * execution transaction without graph/module locks across provider I/O.
 */
int linux_stat_from_path(struct linux_stat_abi *out,const struct vfs_path *path);

/** @brief Dispatch stat/statfs families against retained inode/OFD objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with stable filesystem references.
 * @param number Linux STAT/LSTAT/FSTAT/NEWFSTATAT/STATX/STATFS/FSTATFS number.
 * @param a0 First native ABI argument, pathname or descriptor.
 * @param a1 Second native ABI argument, output or at-pathname.
 * @param a2 Third native ABI argument, output or statx flags.
 * @param a3 Fourth native ABI argument, flags or statx mask.
 * @param a4 Fifth native ABI argument, statx output.
 * @return Zero or negative errno. Retains original objects across provider
 * waits through the common syscall scope; no graph lock spans callbacks.
 */
int64_t syscall_metadata(struct vfs_namespace *ns,struct task *task,uint64_t number,
                           uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4);

/** @brief Dispatch DAC and chmod/chown families using retained inode objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task and stable credentials.
 * @param number Linux access/chmod/chown family syscall number.
 * @param a0 First native ABI argument, pathname or descriptor.
 * @param a1 Second native ABI argument, mode/user ID or at-pathname.
 * @param a2 Third native ABI argument, group ID or at-mode/user ID.
 * @param a3 Fourth native ABI argument, group ID or flags.
 * @param a4 Fifth native ABI argument, fchownat flags.
 * @return Zero or negative errno. Honors the original admitted OFD and the
 * common copy-once path scope; no graph/module lock spans metadata I/O.
 */
int64_t syscall_permissions(struct vfs_namespace *ns,struct task *task,uint64_t number,
                              uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4);

/** @brief Read a held symlink without following its final component.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with retained root/cwd objects.
 * @param dirfd Base descriptor or AT_FDCWD.
 * @param pathname User pathname, copied once across provider waits.
 * @param output User byte output; no NUL is appended.
 * @param capacity Signed Linux buffer capacity; must be positive.
 * @param allow_empty Permit a link descriptor for readlinkat's empty input.
 * @return Copied byte count or negative errno. Returned path/temporary memory
 * are released; common retry scope retains original operands without locks.
 */
int64_t syscall_readlink_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                  uint64_t pathname,uint64_t output,int32_t capacity,bool allow_empty);

/** @brief Truncate an authorized held path or the original admitted OFD.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with retained filesystem/OFD state.
 * @param number Linux TRUNCATE or FTRUNCATE number.
 * @param operand User pathname or signed descriptor respectively.
 * @param length Signed Linux logical length encoded in one register.
 * @return Zero or negative errno. Acquires inode writer admission, preserves
 * seals/readonly and original OFD access rights, and never resolves a display
 * name. Common scope/admission retains objects across I/O without spinlocks.
 */
int64_t syscall_truncate_object(struct vfs_namespace *ns,struct task *task,uint64_t number,
                                  uint64_t operand,uint64_t length);

/** @brief Preallocate the original writable OFD's retained inode backing.
 * @param task Borrowed caller under execution ownership and OFD admission.
 * @param fd Signed user descriptor.
 * @param mode Linux allocation flags as a syscall int.
 * @param offset Signed byte offset encoded in a register.
 * @param length Positive signed byte count encoded in a register.
 * @return Zero or negative errno. Writer exclusion, seals, readonly and
 * allocation guarantees are enforced without replaying diagnostic names;
 * retained paths protect providers across callbacks, without graph locks.
 */
int64_t syscall_fallocate_object(struct task *task,int32_t fd,uint32_t mode,uint64_t offset,uint64_t length);
/** @brief Flush an actual admitted inode, superblock or shared mount tree.
 * @param ns Borrowed initialized namespace under execution ownership.
 * @param task Borrowed caller with original OFD admission.
 * @param number Linux SYNC/SYNCFS/FSYNC/FDATASYNC number.
 * @param fd Signed descriptor, ignored by SYNC.
 * @return Zero or negative errno; SYNC always returns zero after attempting
 * all providers. Others preserve backing I/O errors; O_PATH supports SYNCFS
 * only. No name lookup, graph lock or unpinned provider callback is used.
 */
int64_t syscall_sync_object(struct vfs_namespace *ns,struct task *task,uint64_t number,int32_t fd);
/** @brief Implement mount operations in the one actual referenced namespace.
 * @param ns Borrowed live namespace under execution ownership, required.
 * @param task Calling task with actual root/cwd, required; CAP_SYS_ADMIN checked.
 * @param number Linux MOUNT or UMOUNT2 syscall number.
 * @param a0 User source pathname/label, or UMOUNT2 target pathname.
 * @param a1 User MOUNT target pathname, or low-32-bit UMOUNT2 flags.
 * @param a2 User filesystem type, ignored for remount/move/unmount.
 * @param a3 Native MOUNT flags, ignored for unmount.
 * @param a4 Optional user mount options, ignored for unmount.
 * @return Zero or negative capability/lookup/type/unsupported/busy/I/O errno.
 * Copied inputs and actual operands survive EAGAIN. Completed unmount sync
 * retires its own scoped lookup before busy checking, while a local path
 * protects the provider. No diagnostic path replay or lock across callbacks.
 */
int64_t syscall_mount_control(struct vfs_namespace *ns,struct task *task,uint64_t number,
                                 uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4);

/** @brief Open a pathname against the actual task root/cwd/dirfd objects.
 * @param ns Borrowed system namespace under execution ownership.
 * @param task Borrowed current task and descriptor table.
 * @param dirfd Relative directory fd or AT_FDCWD, ignored for absolute paths.
 * @param pathname User pathname; the active scope retains its checked copy.
 * @param flags Linux open flags, sanitized before private descriptor flags.
 * @param mode Creation permission bits; ignored for existing files.
 * @return New descriptor, negative errno or FIFO blocking sentinel. All OFDs
 * own their actual paths. DAC, exclusive/dangling-link creation, O_PATH and
 * writer admission run without graph/module locks across filesystem I/O.
 */
int64_t syscall_open_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                               uint64_t pathname,uint32_t flags,uint32_t mode);

/** @brief Initialize a device open selected by retained inode identity.
 * @param task Current task under execution ownership.
 * @param path Borrowed authorized device path, independently retained by fd.
 * @param flags Sanitized Linux/native status flags; excludes O_PATH.
 * @param diagnostic Optional display text, never consulted for routing.
 * @return Descriptor or negative errno with balanced driver/path/PTY ownership.
 * Driver callbacks may perform I/O; no graph/module registry lock spans them.
 */
int task_open_device_path(struct task *task,const struct vfs_path *path,uint32_t flags,
                             const char *diagnostic);

/** @brief Translate native filesystem statuses to Linux errno aliases.
 * @param ret Native success/status or negative errno.
 * @return Known Linux errno alias or unchanged ret. Pure conversion for
 * syscall and file-backed page-fault callers; no I/O or ownership transfer.
 */
int storage_errno(int ret);

/** @brief Copy from an original regular OFD to a retained file/stream.
 * @param out_fd_arg Signed low-32-bit writable descriptor.
 * @param in_fd_arg Signed low-32-bit readable regular descriptor.
 * @param offset_arg Optional user signed-off_t input/output pointer.
 * @param count_arg Native requested byte count, capped to Linux MAX_RW_COUNT.
 * @return Bytes, negative errno or internal EAGAIN. Execution ownership is
 * required; both OFDs and staging bytes survive waits without name replay.
 * No graph/module lock spans callbacks, drain or final puts.
 */
int64_t syscall_sendfile(uint64_t out_fd_arg, uint64_t in_fd_arg,
                                uint64_t offset_arg, uint64_t count_arg);

/** @brief Copy nonoverlapping ranges in one actual filesystem instance.
 * @param in_fd_arg Signed low-32-bit readable regular descriptor.
 * @param off_in_arg Optional user signed-off_t source input/output pointer.
 * @param out_fd_arg Signed low-32-bit writable regular descriptor.
 * @param off_out_arg Optional user signed-off_t target input/output pointer.
 * @param len_arg Native requested byte count, capped to Linux MAX_RW_COUNT.
 * @param flags_arg Low-32-bit flags, currently zero; upper bits ignored.
 * @return Bytes, negative errno or internal EAGAIN. Original OFDs, offsets and
 * bytes survive waits. Same inode nonoverlap is permitted; append is EBADF,
 * directory EISDIR, unsupported cross-filesystem copy EXDEV. Execution-owned
 * without graph/module spinlocks across I/O or final references.
 */
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

/**
 * @brief Test an epoll OFD for eligible events without consuming its state.
 * @param task Calling task used for the underlying readiness probes.
 * @param file Epoll open-file description to inspect.
 * @return True when an armed registration is ready, otherwise false.
 * @note Nested epoll registrations are unsupported; this reports only direct
 *       target readiness and preserves edge/one-shot state.
 */
bool task_epoll_poll_ready(struct task *task, struct task_file *file);

int64_t syscall_poll(uint64_t fds_ptr, uint64_t count, int64_t timeout_ms);

/**
 * @brief Create an epoll descriptor with an initially empty dynamic watch set.
 * @param size Linux legacy creation size hint.
 * @param flags Supported creation flags.
 * @return Descriptor or negative Linux errno.
 */
int64_t syscall_epoll_create(uint64_t size, uint32_t flags);

/**
 * @brief Modify an epoll descriptor's watched file set.
 * @param epfd_arg Epoll descriptor number.
 * @param op_arg Linux EPOLL_CTL operation.
 * @param fd_arg Watched descriptor number.
 * @param event_ptr User event pointer for ADD/MOD; unused for DEL.
 * @return Zero on success or negative Linux errno.
 */
int64_t syscall_epoll_ctl(uint64_t epfd_arg, uint64_t op_arg,
                                 uint64_t fd_arg, uint64_t event_ptr);

/**
 * @brief Wait for events from an epoll descriptor.
 * @param epfd_arg Epoll descriptor number.
 * @param events_ptr Writable array of output events.
 * @param maxevents_arg Capacity of the output array.
 * @param timeout_ms Linux millisecond timeout.
 * @return Event count, zero on timeout or negative Linux errno/block token.
 */
int64_t syscall_epoll_wait(uint64_t epfd_arg, uint64_t events_ptr,
                                  uint64_t maxevents_arg, int64_t timeout_ms);

/**
 * @brief Wait using epoll_pwait2's optional timespec and signal-mask arguments.
 * @param epfd Epoll descriptor number.
 * @param events Writable output event array.
 * @param maxevents Output event capacity.
 * @param timeout Optional Linux timespec pointer.
 * @param sigmask Optional signal-mask pointer.
 * @param sigsetsize Signal-mask byte size.
 * @return Event count, zero on timeout or negative Linux errno.
 */
int64_t syscall_epoll_pwait2(uint64_t epfd, uint64_t events, uint64_t maxevents,
                                    uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize);

int64_t syscall_getrusage(uint64_t who_arg, uint64_t usage_ptr);

int64_t syscall_sysinfo(uint64_t info_ptr);

int64_t syscall_times(uint64_t tms_ptr);

/**
 * @brief Implement select/pselect with dynamically sized fd bitmaps.
 * @param nfds_arg Highest descriptor plus one, in the Linux signed-int ABI.
 * @param read_ptr Optional read fd_set pointer.
 * @param write_ptr Optional write fd_set pointer.
 * @param except_ptr Optional exception fd_set pointer.
 * @param timeout_ptr Optional timeval/timespec pointer.
 * @param pselect_timeout True selects a timespec without timeout copyback.
 * @return Ready descriptor count, zero on timeout or negative Linux errno/token.
 */
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

/** @brief Update the exact retained inode's access/modification timestamps.
 * @param task Borrowed calling task with stable filesystem credentials.
 * @param dirfd At-base descriptor, or the inode FD when path_ptr is NULL.
 * @param path_ptr User pathname copied once, or NULL for a futimens FD.
 * @param times Borrowed validated pair with UTIME_NOW/UTIME_OMIT semantics.
 * @param flags AT_SYMLINK_NOFOLLOW/AT_EMPTY_PATH; zero for NULL-path FD mode.
 * @param times_supplied Whether the caller explicitly provided the pair.
 * @return Zero or negative errno. Honors fsuid/CAP_FOWNER or NOW-only write
 * access, preserving the existing backing timestamp resolution. Common scope
 * owns the exact operand across I/O retries; no graph lock spans callbacks.
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

/** @brief Dispatch borrowed raw arguments through the ordinary Linux syscall backends.
 * @param number Linux x86-64 syscall number.
 * @param a0 First raw argument; backend validates any user pointer before access.
 * @param a1 Second raw argument. @param a2 Third raw argument.
 * @param a3 Fourth raw argument. @param a4 Fifth raw argument.
 * @param a5 Sixth raw argument.
 * @return Backend result, negative errno or entry-owned blocking/retry status.
 * Execution-owned task context; no task/file/provider pin escapes this call.
 */
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

/** @brief Enumerate one directory using the selected native Linux record ABI.
 * @param task Current execution-owned task with dispatcher-pinned directory OFD.
 * @param number Linux GETDENTS or GETDENTS64 operation number.
 * @param fd Signed low-32-bit directory descriptor.
 * @param buffer User destination, checked before record emission.
 * @param capacity Low-32-bit available output bytes.
 * @return Accepted bytes, zero at EOF, or negative fd/type/buffer/provider errno.
 * The common dispatcher retains the OFD through waits and final cleanup;
 * no graph/module spinlock spans directory or metadata callbacks.
 * An entry removed before metadata acquisition is skipped with its next cookie;
 * other provider errors preserve that entry and are returned to the caller.
 */
int64_t syscall_getdents_object(struct task *task,uint64_t number,int32_t fd,uint64_t buffer,uint32_t capacity);

/** @brief Seek a pinned regular inode or directory through its actual provider.
 * @param task Execution-owned caller retaining the admitted OFD until cleanup.
 * @param fd Signed low-32-bit descriptor.
 * @param offset Signed byte offset or opaque backend directory position.
 * @param whence Linux SEEK_SET/CUR/END/DATA/HOLE selector.
 * @return New position or negative errno/internal retry; directory cookies are
 * returned unchanged. Metadata waits preserve the OFD reservation, and common
 * cancellation drains I/O before final provider release. No graph lock spans I/O.
 */
int64_t syscall_seek_object(struct task *task,int32_t fd,int64_t offset,uint32_t whence);

/** @brief Decode one Linux timestamp-vector ABI and update a retained inode.
 * @param task Execution-owned caller with optional retry operand scope.
 * @param number UTIME/UTIMES/FUTIMESAT/UTIMENSAT operation number.
 * @param a0 First native argument, pathname or directory descriptor.
 * @param a1 Second native argument, time vector or pathname.
 * @param a2 Third native argument, at-family time vector.
 * @param a3 Fourth native argument, UTIMENSAT lookup flags.
 * @return Zero or negative errno/internal retry. The common scope owns copied
 * binary bytes and exact lookup paths through waits, including user unmap/name
 * reuse. Unscoped snapshots are freed on every outcome. No graph/module lock
 * spans copying or actual provider I/O.
 */
int64_t syscall_timestamp_object(struct task *task,uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3);

/** @brief Dispatch one Linux vectored request through the admitted OFD.
 * @param task Borrowed execution-owned caller retaining its original OFD.
 * @param number READV/WRITEV/PREADV/PWRITEV/PREADV2/PWRITEV2 syscall number.
 * @param a0 Signed low32bit descriptor argument.
 * @param a1 User vector array address.
 * @param a2 Vector count, checked against UIO_MAXIOV.
 * @param a3 Full64bit position for positional variants.
 * @param a4 Unused x86_64 pos_h argument, ignored.
 * @param a5 Low32bit v2 RWF flags, ignored by older variants.
 * @return Accepted bytes, negative errno or internal retry. Ordinary files
 * retain imported vectors/progress; specialized devices keep their own APIs.
 * Caller retains the OFD across waits, without graph/module locks across I/O.
 */
int64_t syscall_vector_io(struct task *task,uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,
                            uint64_t a3,uint64_t a4,uint64_t a5);

#endif

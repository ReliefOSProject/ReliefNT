/* Private kernel task/resource layouts. No user ABI definitions. */
#ifndef RELIEFNT_TASK_TYPES_H
#define RELIEFNT_TASK_TYPES_H

#include <reliefnt/paging.h>
#include <reliefnt/signal.h>
#include <reliefnt/storage.h>
#include <reliefnt/trap.h>
#include <reliefnt/types.h>
#include <reliefnt/eevdf.h>
#include <reliefnt/heap.h>
#include <reliefos/auth_abi.h>
#include <reliefos/elf_abi.h>
#include <linux/resource.h>
#include <linux/time.h>
#include <linux/uio.h>
#include <linux/capability.h>
#include <linux/sem.h>

struct audio_file;
struct audio_control_file;
struct audio_oss_file;
struct audio_mixer_file;
struct audio_pcm;
struct drm_file;
struct drm_bo;
struct drm_prime_file;

#define SCHED_TASK_NAME_LEN 32u
/* Initial task-table capacity; the scheduler grows beyond this value. */
#define SCHED_TASK_MAX 64u
#define SCHED_TASK_FILE_MAX 64u
#define SCHED_TASK_FILE_LIMIT 1024u
#define SCHED_NR_OPEN 1048576u
#define SCHED_TASK_PTY_FD_MAX 8u
#define SCHED_TASK_STDIO_MAX 3u
/* Linux MAX_ARG_STRINGS / MAX_ARG_STRLEN; the total budget follows RLIMIT_STACK. */
#define SCHED_EXEC_ARG_MAX 0x7fffffffu
#define SCHED_EXEC_ENV_MAX 0x7fffffffu
#define SCHED_EXEC_STR_MAX (32u * 4096u)
#define SCHED_EXEC_DATA_MAX (6u * 1024u * 1024u)

/**
 * @brief Compute Linux's max(128 KiB, min(6 MiB, soft stack limit / 4)).
 * @param stack_limit Current soft RLIMIT_STACK, including RLIM_INFINITY.
 * @return Combined string and pointer budget in bytes.
 */
static inline uint32_t sched_exec_argument_limit(uint64_t stack_limit)
{
    uint64_t limit = stack_limit / 4;
    if (limit > SCHED_EXEC_DATA_MAX) limit = SCHED_EXEC_DATA_MAX;
    if (limit < SCHED_EXEC_STR_MAX) limit = SCHED_EXEC_STR_MAX;
    return (uint32_t)limit;
}
#define SCHED_TASK_VMA_MAX 128u
#define SCHED_TASK_TIMER_MAX 32u

#define TASK_VMA_FLAG_PRIVATE 0x00000001u
#define TASK_VMA_FLAG_ANON    0x00000002u
#define TASK_VMA_FLAG_FILE    0x00000004u
#define TASK_VMA_FLAG_LAZY    0x00000008u
#define TASK_VMA_FLAG_SHARED_FILE 0x00000010u
#define TASK_VMA_FLAG_DYNAMIC_LOAD 0x00000020u
#define TASK_VMA_FLAG_DEVICE  0x00000040u
#define TASK_VMA_FLAG_SHARED  0x00000080u
#define TASK_VMA_FLAG_LOCKED  0x00000100u
#define TASK_VMA_FLAG_MMAP    0x00000200u
#define TASK_VMA_FLAG_SYSV_SHM 0x00000400u
#define TASK_VMA_FLAG_VDSO    0x00000800u
#define TASK_VMA_FLAG_VVAR    0x00001000u

#define TASK_VMA_PROT_READ  0x1u
#define TASK_VMA_PROT_WRITE 0x2u
#define TASK_VMA_PROT_EXEC  0x4u

struct task_vma {
    uint32_t used;
    uint32_t prot;
    uint32_t max_prot;
    uint32_t flags;
    uint32_t reserved;
    uint64_t start;
    uint64_t end;
    uint64_t file_offset;
    uint64_t file_limit;
    /* Physical base of device-backed mappings (shmem, framebuffer, DRM BO);
     * zero for ordinary file and anonymous mappings. */
    uint64_t device_physical;
    struct storage_node file_node;
    struct storage_inode_ref *inode;
    struct audio_pcm *audio_pcm;
    /* A DRM VMA retains its BO and the source DRM open-file description.
     * The latter preserves master/auth state after numeric fd close. */
    struct drm_bo *drm_bo;
    struct task_file *drm_mapping_file;
    struct sysv_shm_attachment *sysv_shm_attachment;
    uint32_t audio_mmap_region;
    uint32_t audio_mmap_generation;
    uint32_t audio_mmap_prot;
    uint32_t audio_mmap_reserved;
    uint64_t audio_mmap_offset;
    uint64_t audio_mmap_length;
};

struct task_file {
    struct task_file *description;
    uint32_t references;
    uint32_t used;
    uint32_t scm_references;
    uint32_t scm_gc_seen;
    uint32_t flags;
    uint32_t fd_flags;
    uint32_t flock_type;
    uint32_t kind;
    uint32_t io_owner;
    struct storage_node node;
    struct storage_inode_ref *inode;
    uint64_t offset;
    uint64_t aux;
    /* Per-descriptor device state; currently the evdev EVIOCGRAB token. */
    uint64_t aux2;
    /* Standard PCM device state owned by the open file description. */
    struct audio_file *audio_file;
    /* ALSA control state owned by the open file description. */
    struct audio_control_file *audio_control_file;
    /* OSS /dev/dsp state owned by the open file description. */
    struct audio_oss_file *audio_oss_file;
    /* OSS /dev/mixer state owned by the open file description. */
    struct audio_mixer_file *audio_mixer_file;
    /* ALSA /dev/snd/timer state owned by the open file description. */
    struct audio_timer_file *audio_timer_file;
    /* DRM primary-node state owned by the open file description. */
    struct drm_file *drm_file;
    /* Opaque same-device vmwgfx surface PRIME file; also OFD-owned. */
    struct drm_prime_file *drm_prime_file;
    uint64_t drm_prime_ino;
    uint64_t drm_prime_size;
    uint32_t drm_prime_uid;
    uint32_t drm_prime_gid;
    uint32_t input_vt; /* evdev graphical-VT filter, zero for the raw stream. */
    uint8_t input_vt_manual; /* EVIOCSVT explicitly selected the filter mode. */
    struct storage_read_cursor read_cursor;
    char path[RELIEFOS_FS_PATH_LEN];
};

#define TASK_FILE_KIND_SIGNALFD 1u
#define TASK_FILE_KIND_DRM_PRIME 4u

static inline struct task_file *task_file_description(struct task_file *file)
{
    return file && file->description ? file->description : file;
}

#define TASK_FILE_FLAG_PIPE       0x80000000u
#define TASK_FILE_FLAG_PIPE_WRITE 0x40000000u
#define TASK_FILE_FLAG_DEV_NULL   0x20000000u
#define TASK_FILE_FLAG_DEV_NODE   0x10000000u
#define TASK_FILE_FLAG_DEV_BLOCK  0x02000000u
#define TASK_FILE_FLAG_SOCKET     0x08000000u
#define TASK_FILE_FLAG_SOCKET_UNIX 0x04000000u
#define TASK_FILE_FLAG_DEV_SHM    0x01000000u
#define TASK_FILE_FLAG_SOCKET_INET 0x00800000u
#define TASK_FILE_FLAG_EVENTFD     0x00000010u
#define TASK_FILE_FLAG_EPOLL       0x00200000u
/* Keep private kinds outside Linux O_SYNC (0x101000) and O_TMPFILE
 * (0x410000). User status flags remain intact for F_GETFL and I/O policy. */
#define TASK_FILE_FLAG_TIMERFD     0x00000008u
/* Linux FMODE_PATH: the descriptor came from open(O_PATH) and therefore only
 * holds a path reference.  The raw LINUX_O_PATH bit cannot remain in
 * file->flags because it is numerically identical to TASK_FILE_FLAG_EPOLL, so
 * the open path translates it into this private bit. */
#define TASK_FILE_FLAG_PATH        0x00000004u

struct task_pty_description {
    uint32_t references;
    uint32_t status_flags;
    uint32_t pty_id;
    uint32_t endpoint;
};

/* Aliases of the standard streams for a process attached to a PTY. */
struct task_pty_fd {
    uint32_t used;
    int32_t fd;
    uint32_t stream;
    /* Descriptor flags: currently only FD_CLOEXEC. */
    uint32_t flags;
    /* Initial flags before promotion; shared flags live in description. */
    uint32_t status_flags;
    /* Unix98 PTY endpoints use an explicit session and direction. Legacy
     * aliases leave pty_id/endpoint zero and continue using stream. */
    uint32_t pty_id;
    uint32_t endpoint;
    struct task_pty_description *description;
};

#define TASK_PTY_ENDPOINT_MASTER 1u
#define TASK_PTY_ENDPOINT_SLAVE  2u

enum task_state {
    TASK_READY = 0,
    TASK_RUNNING = 1,
    TASK_BLOCKED = 2,
    TASK_EXITED = 3,
    /* A SIGSTOPped task keeps its address space and descriptors but is not
     * eligible for scheduling until SIGCONT makes it ready again. */
    TASK_STOPPED = 4,
};

enum task_kind {
    TASK_KIND_KERNEL = 0,
    TASK_KIND_USER = 1,
};

#define TASK_FLAG_SERVICE 0x00000001u
#define TASK_FLAG_STARTED 0x00000002u
#define TASK_FLAG_RESOURCES_RELEASED 0x00000004u
#define TASK_FLAG_ELEVATED_ADMIN 0x00000010u
#define TASK_FLAG_PENDING_LOAD 0x00000020u
/* Real fork children remain zombies until their parent calls waitpid. */
#define TASK_FLAG_WAITABLE_CHILD 0x00000040u
/* A wait4 caller has claimed this zombie and is releasing it outside the
 * scheduler lock.  It prevents a second waiter from recycling the same slot. */
#define TASK_FLAG_REAP_IN_PROGRESS 0x00000080u
#define TASK_FLAG_FORK_NOEXEC 0x00000100u
#define TASK_FLAG_ORPHAN_NOTIFY 0x00000200u
#define TASK_FLAG_PDEATH_NOTIFY 0x00000400u

/* A stopped or continued child retains its task slot until its parent has
 * observed the state transition through waitpid. */
#define TASK_CHILD_EVENT_NONE      0x00000000u
#define TASK_CHILD_EVENT_STOPPED   0x00000001u
#define TASK_CHILD_EVENT_CONTINUED 0x00000002u

/* The task object is split into ownership domains. Anonymous compatibility
 * views preserve task->field while code migrates to named substructures. */
struct task_process_state {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t process_group;
    uint32_t process_session;
    char name_storage[SCHED_TASK_NAME_LEN];
    const char *name;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t stack_low;
    uint64_t fs_base;
    uint64_t clear_child_tid;
    uint64_t wake_tick;
    uint64_t poll_deadline_ticks;
    uint64_t exit_code;
    enum task_state state;
    enum task_kind kind;
    uint32_t flags;
    uint64_t cpu_ticks;
    int32_t priority;
    /* CPUs on which this task may execute. A zero mask is treated as all
     * discovered CPUs for compatibility with older task initializers. */
    uint64_t affinity_mask;
    uint32_t mlockall_flags;
};

struct task_address_space_state {
    struct address_space as;
    struct task_vma vmas[SCHED_TASK_VMA_MAX];
    struct task_vma *vma_extra;
    uint32_t vma_extra_count;
    uint32_t vma_extra_capacity;
    /* Linux brk(2) state belongs to the address space, shared by CLONE_VM. */
    uint64_t program_break_base;
    uint64_t program_break;
    uint32_t references;
    uint64_t initial_stack_top;
    uint64_t initial_stack_low;
    uint64_t arg_start, arg_end, env_start, env_end;
    /* Zero-initialized new/exec address spaces are dumpable by default. */
    bool nondumpable;
    struct storage_inode_ref *executable_inode;
    /* Linux membarrier registration commands are process/MM scoped. */
    uint32_t membarrier_registrations;
};

struct task_fd_table_state {
    struct task_file files[SCHED_TASK_FILE_MAX];
    struct task_file stdio_files[SCHED_TASK_STDIO_MAX];
    /* Bits for closed implicit stdin/stdout/stderr descriptors. */
    uint32_t closed_stdio_mask;
    /* Bits for implicit stdio descriptors marked close-on-exec by
     * close_range(CLOSE_RANGE_CLOEXEC). */
    uint32_t cloexec_stdio_mask;
    struct task_file *file_extra;
    uint32_t file_extra_count;
    uint32_t file_extra_capacity;
    struct task_pty_fd pty_fds[SCHED_TASK_PTY_FD_MAX];
    uint32_t references;
    uint64_t lock_owner;
};

struct task_fs_state {
    uint32_t umask;
    char cwd[RELIEFOS_FS_PATH_LEN];
    char root_dir[RELIEFOS_FS_PATH_LEN];
    uint32_t references;
};

struct task_sighand_state {
    struct kernel_signal_action actions[KERNEL_SIGNAL_ACTION_MAX];
    uint32_t references;
};

struct task_signal_state {
    uint64_t pending_signals;
    uint64_t timer_pending_signals;
    uint64_t blocked_signals;
    uint64_t sigsuspend_saved_mask;
    uint32_t sigsuspend_active;
    uint32_t child_event;
    uint32_t stop_signal;
    uint32_t exit_signal;
    uint32_t parent_exit_signal;
    uint32_t parent_exit_notified;
    /* Linux task_struct::pdeath_signal, plus the creating-thread link that
     * matches real_parent for parent-death delivery (see copy_process). */
    uint32_t pdeath_signal;
    uint32_t pdeath_parent_pid;
    uint64_t ignored_signals;
};

/* Linux POSIX timers are per-process objects, but the native task table is
 * also the ownership boundary for this kernel.  Timer ids are small integers
 * scoped to the task that created them, matching the ids exposed by musl. */
struct task_posix_timer {
    uint32_t used;
    uint32_t id;
    int32_t clockid;
    int32_t notify;
    int32_t signo;
    int32_t target_pid;
    uint64_t value;
    uint64_t expiry_tick;
    uint64_t interval_ticks;
    uint64_t overrun;
    uint64_t value_data;
};

struct task_groups {
    uint32_t references;
    uint32_t count;
    uint32_t ids[];
};

struct task_credentials_state {
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t fsuid;
    uint32_t fsgid;
    struct task_groups *groups;
    uint32_t role;
    uint32_t session_id;
    uint32_t umask;
    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t cap_bset;
    uint64_t cap_ambient;
    uint32_t securebits;
    char username[RELIEFOS_AUTH_USERNAME_LEN];
    char home[RELIEFOS_AUTH_HOME_LEN];
    char cwd[RELIEFOS_FS_PATH_LEN];
    char root_dir[RELIEFOS_FS_PATH_LEN];
};

struct task_terminal_state {
    uint32_t pty_id;
    uint32_t wait_pty_id;
    uint32_t controlling_pty_id;
};

struct task_loader_state {
    const void *image;
    size_t image_len;
    struct storage_node image_node;
    char path[RELIEFOS_FS_PATH_LEN];
    uint32_t exec_argc;
    uint32_t exec_envc;
    uint32_t exec_data_len;
    uint16_t exec_phnum;
    bool secure_exec;
    uint16_t exec_reserved;
    char **exec_argv;
    char **exec_envp;
    char *exec_data;
    struct reliefos_dynamic_launch dynamic_launch;
};

struct task_rlimit_state {
    uint32_t references;
    struct linux_rlimit64 nofile;
    struct linux_rlimit64 as;
    struct linux_rlimit64 sigpending;
    struct linux_rlimit64 stack;
    struct linux_rlimit64 nproc;
    struct linux_rlimit64 core;
    struct linux_rlimit64 memlock;
};

struct sysv_sem_array;
struct sysv_sem_wait_list;
struct sysv_sem_undo;
struct sysv_sem_undo_list;
struct task_sysv_sem_state {
    struct task_sysv_sem_state *previous, *next;
    struct sysv_sem_wait_list *list;
    struct sysv_sem_array *array;
    struct sysv_sem_undo *undo;
    struct task *task;
    struct linux_sembuf *ops;
    struct linux_sembuf fast_ops[64];
    uint64_t deadline;
    uint32_t count, blocking, number;
    int32_t pid, result;
    bool alter, timed, completed;
};

struct sysv_msg_queue;
struct sysv_message;
struct task_sysv_msg_state {
    struct task_sysv_msg_state *previous, *next;
    struct sysv_msg_queue *queue;
    struct sysv_message *message;
    struct task *task;
    uint64_t buffer, size;
    int64_t type;
    uint32_t flags, number;
    int32_t result;
    bool linked, completed;
};

struct task_mmsg_state {
    uint64_t vector, timeout_pointer;
    struct linux_timespec deadline, remaining;
    uint32_t count, length, flags;
    bool active, receiving;
};

struct task {
    struct eevdf_entity fair;
    uint64_t start_uptime_ms;
    bool no_new_privs;
    bool nproc_exceeded;
    struct task_rlimit_state limits;
    struct task_rlimit_state *shared_limits;
    uint32_t tgid;
    uint64_t process_pending_signals;
    uint64_t *shared_process_pending;
    struct kernel_sigqueue signal_queue, process_signal_queue;
    struct kernel_sigqueue *shared_process_signal_queue;
    uint64_t alarm_deadline;
    uint64_t alarm_interval_ns;
    uint64_t alarm_last_expiry;
    struct task_address_space_state *shared_mm;
    /* CLONE_VFORK links: the child points at its waiting parent and the
     * parent points at the child whose exec/exit releases it.  Both fields
     * are zero for ordinary processes and are maintained under
     * scheduler_lock. */
    struct task *vfork_parent;
    struct task *vfork_child;
    struct task_fd_table_state *shared_files;
    struct task_fs_state *shared_fs;
    struct task_sighand_state *shared_sighand;
    uint64_t robust_list;
    uint64_t rseq_area;
    uint32_t rseq_len;
    uint32_t rseq_sig;
    uint64_t signal_stack_base;
    uint64_t signal_stack_size;
    uint32_t signal_stack_flags;
    uint64_t restart_syscall;
    uint64_t nanosleep_deadline;
    uint64_t nanosleep_remaining;
    int32_t nanosleep_clock;
    uint64_t sigwait_deadline;
    uint64_t sigwait_mask;
    uint64_t signalfd_wait_mask;
    bool signalfd_waiting;
    void *signalfd_vectors;
    struct iovec signalfd_fast_vectors[8];
    uint64_t signalfd_vector_bytes;
    uint32_t signalfd_vector_count, signalfd_read_flags;
    uint32_t sigwait_active;
    struct task_posix_timer timers[SCHED_TASK_TIMER_MAX];
    struct kernel_wait_queue *waiting_queue;
    struct task_file *socket_receive_file;
    uint64_t socket_receive_done;
    int32_t socket_receive_pid;
    uint32_t socket_receive_uid;
    uint32_t socket_receive_gid;
    uint64_t socket_receive_message;
    uint64_t socket_receive_control_capacity;
    uint32_t socket_receive_name_capacity;
    uint32_t socket_receive_flags, socket_receive_path_length;
    uint64_t socket_receive_name;
    unsigned char socket_receive_path[111];
    struct task_file *syscall_file;
    /* Task-owned vmwgfx ioctl continuation; opaque to the scheduler. */
    void *drm_vmwgfx_continuation;
    struct task_file *fifo_open_file; /* Unpublished endpoint held during FIFO rendezvous. */
    uint32_t tty_old_pgrp;
    struct task_pty_fd syscall_pty;
    int32_t syscall_fd;
    uint32_t syscall_file_number;
    struct {
        uint64_t buffer, count, position, done;
        bool active;
    } regular_io;
    uint64_t socket_io_deadline;
    bool socket_io_timed;
    struct task_mmsg_state mmsg;
    struct task_sysv_msg_state sysv_msg;
    struct task_sysv_sem_state sysv_sem;
    struct sysv_sem_undo_list *sysv_undo;
    struct task *futex_next;
    uint64_t futex_key;
    uint64_t futex_domain;
    uint64_t futex_address;
    uint64_t futex_deadline;
    uint32_t futex_bitset;
    int32_t futex_state;
#define SCHED_FUTEX_WAITV_MAX 128
    uint32_t futex_waitv_count;
    uint32_t futex_waitv_index;
    uint64_t futex_waitv_domain[SCHED_FUTEX_WAITV_MAX];
    uint64_t futex_waitv_key[SCHED_FUTEX_WAITV_MAX];
    union {
        struct task_process_state process;
        struct {
            uint32_t pid;
            uint32_t parent_pid;
            uint32_t process_group;
            uint32_t process_session;
            char name_storage[SCHED_TASK_NAME_LEN];
            const char *name;
            uint64_t entry;
            uint64_t stack_top;
            uint64_t stack_low;
            uint64_t fs_base;
            uint64_t clear_child_tid;
            uint64_t wake_tick;
            uint64_t poll_deadline_ticks;
            uint64_t exit_code;
            enum task_state state;
            enum task_kind kind;
            uint32_t flags;
            uint64_t cpu_ticks;
            int32_t priority;
            uint64_t affinity_mask;
            uint32_t mlockall_flags;
        };
    };
    union {
        struct task_address_space_state address_space;
        struct {
            struct address_space as;
            struct task_vma vmas[SCHED_TASK_VMA_MAX];
            struct task_vma *vma_extra;
            uint32_t vma_extra_count;
            uint32_t vma_extra_capacity;
            uint64_t program_break_base;
            uint64_t program_break;
        };
    };
    union {
        struct task_fd_table_state fd_table;
        struct {
            struct task_file files[SCHED_TASK_FILE_MAX];
            struct task_file stdio_files[SCHED_TASK_STDIO_MAX];
            uint32_t closed_stdio_mask;
            struct task_file *file_extra;
            uint32_t file_extra_count;
            uint32_t file_extra_capacity;
            struct task_pty_fd pty_fds[SCHED_TASK_PTY_FD_MAX];
            uint32_t file_references;
            uint64_t lock_owner;
        };
    };
    union {
        struct task_signal_state signal_state;
        struct {
            uint64_t pending_signals;
            uint64_t timer_pending_signals;
            uint64_t blocked_signals;
            uint64_t sigsuspend_saved_mask;
            uint32_t sigsuspend_active;
            uint32_t child_event;
            uint32_t stop_signal;
            uint32_t exit_signal;
            uint32_t parent_exit_signal;
            uint32_t parent_exit_notified;
            /* Linux task_struct::pdeath_signal, plus the creating-thread link that
             * matches real_parent for parent-death delivery (see copy_process). */
            uint32_t pdeath_signal;
            uint32_t pdeath_parent_pid;
            uint64_t ignored_signals;
        };
    };
    union {
        struct task_credentials_state credentials;
        struct {
            uint32_t uid;
            uint32_t gid;
            uint32_t euid;
            uint32_t egid;
            uint32_t suid;
            uint32_t sgid;
            uint32_t fsuid;
            uint32_t fsgid;
            struct task_groups *groups;
            uint32_t role;
            uint32_t session_id;
            uint32_t umask;
            uint64_t cap_effective;
            uint64_t cap_permitted;
            uint64_t cap_inheritable;
            uint64_t cap_bset;
            uint64_t cap_ambient;
            uint32_t securebits;
            char username[RELIEFOS_AUTH_USERNAME_LEN];
            char home[RELIEFOS_AUTH_HOME_LEN];
            char cwd[RELIEFOS_FS_PATH_LEN];
            char root_dir[RELIEFOS_FS_PATH_LEN];
        };
    };
    union {
        struct task_terminal_state terminal_state;
        struct {
            uint32_t pty_id;
            uint32_t wait_pty_id;
            uint32_t controlling_pty_id;
        };
    };
    union {
        struct task_loader_state loader_state;
        struct {
            const void *image;
            size_t image_len;
            struct storage_node image_node;
            char path[RELIEFOS_FS_PATH_LEN];
            uint32_t exec_argc;
            uint32_t exec_envc;
            uint32_t exec_data_len;
            uint16_t exec_phnum;
            bool secure_exec;
            uint16_t exec_reserved;
            char **exec_argv;
            char **exec_envp;
            char *exec_data;
            struct reliefos_dynamic_launch dynamic_launch;
        };
    };
    struct trap_frame frame;
    uint64_t signal_fault_address;
    uint32_t page_fault_signal;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    struct kernel_signal_action signal_actions[KERNEL_SIGNAL_ACTION_MAX];
    uint32_t running_cpu;
    uint32_t last_cpu;
};


#endif

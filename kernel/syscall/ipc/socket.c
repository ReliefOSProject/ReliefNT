/* Unix-domain sockets backed by the kernel object table. */
#include <reliefnt/heap.h>
#include <reliefnt/proc_stats.h>
#include "unix_path.h"
#include <reliefnt/net.h>
#include <reliefnt/net_udp.h>
#include <reliefnt/net_packet.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/socket_usercopy.h>
#include <reliefnt/swap.h>
#include <reliefnt/wait.h>
#include <reliefnt/futex.h>
#ifdef CONFIG_SWAP
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#endif
#include <reliefnt/time.h>
#include <reliefnt/permissions.h>
#include <reliefnt/pty.h>
#include <reliefos/fs_abi.h>
#include <reliefos/net_abi.h>
#include <linux/socket.h>
#include <linux/if_packet.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/tty.h>
#include <reliefnt/device_mapping.h>

#ifdef CONFIG_SWAP
extern void page_cache_note_write(uint64_t) __attribute__((weak));
#endif

#define ntohs(value) ((uint16_t)((((uint16_t)(value) & 0xffu) << 8) | ((uint16_t)(value) >> 8)))
#define htons(value) ntohs(value)
#define ntohl(value) ((((uint32_t)(value) & 0xffu) << 24) | (((uint32_t)(value) & 0xff00u) << 8) | (((uint32_t)(value) & 0xff0000u) >> 8) | ((uint32_t)(value) >> 24))
#define htonl(value) ntohl(value)

#define UNIX_SOCKET_BACKLOG_MAX 4096u
#define UNIX_SOCKET_PENDING_MAX (UNIX_SOCKET_BACKLOG_MAX + 1u)
#define UNIX_SOCKET_RX_CAP 16384u
#define UNIX_SOCKET_FD_TRANSFER_MAX 253u
#define UNIX_SOCKET_CMSG_CAP 65536u

#ifdef CONFIG_SWAP
/** @brief Retire this TCP socket's readiness wait before a prerequisite wait.
 * @param task Current execution owner. @param file Pinned socket description.
 * @param result Current result, returned unchanged. Other waits are untouched.
 */
static int64_t inet_user_wait(struct task *task,struct task_file *file,int64_t result)
{
    if(result==KERNEL_SYSCALL_SWAP_CONTENDED && file &&
       (file->flags & TASK_FILE_FLAG_SOCKET_INET) &&
       file->kind!=TASK_FILE_KIND_UDP && file->kind!=TASK_FILE_KIND_PACKET)
        net_socket_cancel_wait((int32_t)file->aux,task);
    return result;
}

/** @brief Check an original syscall byte span without touching its pages.
 * @param address Original user pointer. @param length Original byte count.
 * @return True when empty or wholly inside the user address aperture.
 */
static bool inet_user_span(uint64_t address,uint64_t length)
{
    return !length || (address>=RELIEFNT_USER_BASE && address<RELIEFNT_USER_TOP &&
                       length<=RELIEFNT_USER_TOP-address);
}
#endif

struct unix_rights {
    struct unix_rights *next;
    uint64_t position;
    uint64_t end;
    uint32_t count;
    bool detached;
    struct task_file *files[UNIX_SOCKET_FD_TRANSFER_MAX];
    struct task_pty_fd ptys[UNIX_SOCKET_FD_TRANSFER_MAX];
};

struct unix_packet {
    struct unix_packet *next;
    uint32_t length;
    uint32_t path_len;
    struct ucred credentials;
    char path[109];
    unsigned char data[];
};

struct unix_receive_result {
    uint32_t flags;
    uint32_t barrier;
    uint32_t path_len;
    struct ucred credentials;
    char path[109];
};

struct unix_stream_credentials {
    struct unix_stream_credentials *next;
    uint64_t start, end;
    struct ucred credentials;
};

enum unix_socket_state { UNIX_SOCKET_OPEN = 0, UNIX_SOCKET_LISTEN = 1,
                         UNIX_SOCKET_CONNECTED = 2 };

struct unix_socket {
    uint32_t used;
    uint32_t refs;
    uint32_t gc_roots;
    uint32_t gc_mark;
    uint32_t handle;
    uint32_t owner_pid;
    uint32_t state;
    uint32_t type;
    uint32_t peer_handle;
    uint32_t peer_pid;
    uint32_t shutdown_read;
    uint32_t shutdown_write;
    int32_t error;
    uint32_t passcred;
    uint32_t receive_lowwater;
    bool bound;
    uint32_t unaccepted;
    uint64_t receive_timeout;
    uint64_t send_timeout;
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t pending_count;
    uint32_t pending_capacity;
    uint32_t backlog; /* Normalized unsigned Linux listen backlog, capped at 4096. */
    uint32_t pending_limit; /* AF_UNIX may queue backlog plus one connection. */
    uint32_t *pending;
    uint64_t rx_read;
    uint64_t rx_written;
    struct unix_rights *rights;
    struct unix_stream_credentials *cred_head, *cred_tail;
    struct unix_packet *packets;
    struct unix_packet *packet_tail;
    uint32_t packet_count;
    uint32_t packet_bytes;
    char path[109];
    uint32_t path_len;
    char peer_path[109];
    uint32_t peer_path_len;
    struct vfs_path bound_inode; /* Owned until final socket destruction. */
    struct ucred credentials;
    struct ucred peer_credentials;
    uint8_t rx[UNIX_SOCKET_RX_CAP];
    struct kernel_wait_queue wait_rx;
    struct kernel_wait_queue wait_tx;
    struct kernel_wait_queue wait_accept;
    struct kernel_wait_queue wait_connect;
    struct unix_socket *registry_next;
};

static struct unix_socket *unix_sockets;

/**
 * @brief Grow a listener's pending handle array up to backlog plus one entry.
 * @param socket Listener that owns the array.
 * @param needed Number of pending handles that must fit.
 * @return Zero on success, negative ENOMEM on allocation failure, or negative
 *         EINVAL when needed exceeds the listener's pending limit.
 */
static int unix_pending_reserve(struct unix_socket *socket, uint32_t needed)
{
    uint32_t capacity;
    uint32_t *pending;
    if (needed <= socket->pending_capacity) return 0;
    if (needed > socket->pending_limit || needed > UNIX_SOCKET_PENDING_MAX)
        return -RELIEFOS_EINVAL;
    capacity = socket->pending_capacity ? socket->pending_capacity : 8u;
    while (capacity < needed) {
        if (capacity > UNIX_SOCKET_PENDING_MAX / 2u) {
            capacity = UNIX_SOCKET_PENDING_MAX;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > socket->pending_limit) capacity = socket->pending_limit;
    pending = kernel_malloc((size_t)capacity * sizeof(*pending));
    if (!pending) return -RELIEFOS_ENOMEM;
    for (uint32_t i = 0; i < socket->pending_count; ++i)
        pending[i] = socket->pending[i];
    kernel_free(socket->pending);
    socket->pending = pending;
    socket->pending_capacity = capacity;
    return 0;
}

static int unix_block(struct unix_socket *socket, struct task_file *file,
                       struct kernel_wait_queue *queue, bool receiving, uint32_t flags)
{
    struct task *task = sched_current_task();
    if ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -RELIEFOS_EAGAIN;
    uint64_t now = time_ticks();
    if (!task->socket_io_deadline) {
        uint64_t timeout = receiving ? socket->receive_timeout : socket->send_timeout;
        task->socket_io_timed = timeout != UINT64_MAX;
        task->socket_io_deadline = UINT64_MAX - now < timeout ? UINT64_MAX : now + timeout;
    }
    if (task->socket_io_deadline <= now) return -RELIEFOS_EAGAIN;
    if (sched_task_pending(task) & ~task->blocked_signals) return KERNEL_SYSCALL_BLOCKED;
    if (kernel_wait_queue_add(queue, task) < 0) return -LINUX_ENOBUFS;
    if (task->socket_io_deadline != UINT64_MAX) sched_sleep_current_until(task->socket_io_deadline);
    else sched_block_current();
    return KERNEL_SYSCALL_BLOCKED;
}

static bool unix_credentials_equal(struct ucred left, struct ucred right)
{
    return left.pid == right.pid && left.uid == right.uid && left.gid == right.gid;
}

static struct ucred unix_send_credentials(struct unix_socket *socket,
                                         struct unix_socket *peer,
                                         const struct ucred *explicit_credentials)
{
    if (explicit_credentials) return *explicit_credentials;
    struct task *task = sched_current_task();
    if (socket->passcred || peer->passcred || peer->unaccepted)
        return (struct ucred){(int32_t)sched_task_tgid(task), task->uid, task->gid};
    return (struct ucred){0, 65534, 65534};
}

static void unix_rights_free(struct unix_rights *rights)
{
    if (!rights) return;
    for (uint32_t i = 0; i < rights->count; ++i) {
        if (!rights->files[i]) {
            task_pty_release_entry(&rights->ptys[i]);
            continue;
        }
        if(!rights->detached)--rights->files[i]->scm_references;
        task_file_put(rights->files[i]);
    }
    kernel_free(rights);
}

#ifdef CONFIG_SWAP
/** @brief Move rights between a task root and the socket's queued edge set.
 * @param rights Owned list, nullable. @param detached True for task ownership.
 * Each entry is adjusted exactly once, including repeated references to the
 * same OFD. The execution writer excludes concurrent socket graph collection.
 */
static void unix_rights_detach(struct unix_rights *rights,bool detached)
{
    if(!rights || rights->detached==detached)return;
    for(uint32_t i=0;i<rights->count;++i)if(rights->files[i]) {
        if(detached)--rights->files[i]->scm_references;
        else ++rights->files[i]->scm_references;
    }
    rights->detached=detached;
}

/* Small output regions are pinned independently; copied packet bytes are
 * streamed to user pages and never pin the entire payload. */
#define SOCKET_OUTPUT_REGIONS 5u
#define SOCKET_OUTPUT_PINS 10u
struct socket_output_region {
    uint64_t address;
    uint32_t length,pinned;
};
struct socket_output_pin {
    uint64_t physical;
    uint16_t offset,length;
    uint8_t region;
    bool cache;
    bool device;
    struct device_user_page device_page;
    struct paging_writable_alias alias;
};

/* The admitted OFD and copied operands outlive this private stage. */
struct socket_send_stage {
    struct task_file *file;
    struct unix_rights *rights;
    struct ucred credentials;
    uint64_t number,args[6];
    bool explicit_credentials;
    bool receiving,accepted,progress_done,outputs_planned,passcred;
    uint8_t *data;
    uint32_t data_length,copied,pin_count;
    uint64_t prior_done;
    uint64_t receive_header,name_address,name_length_pointer;
    uint32_t name_capacity;
    int receive_result;
    struct unix_receive_result metadata;
    struct socket_output_region regions[SOCKET_OUTPUT_REGIONS];
    struct socket_output_pin pins[SOCKET_OUTPUT_PINS];
};

/** @brief Free a detached private stage and all of its retained resources.
 * @param task Execution-owned owner. @param stage Detached stage, required.
 */
static void socket_stage_dispose(struct task *task,struct socket_send_stage *stage)
{
    for(uint32_t i=0;i<stage->pin_count;++i) {
        if(stage->pins[i].device)device_user_page_put(&stage->pins[i].device_page);
        else {
            paging_writable_alias_remove(&stage->pins[i].alias);
            if(stage->pins[i].cache)page_cache_release(stage->pins[i].physical);
            else mm_free_page(stage->pins[i].physical);
        }
    }
    kernel_free(stage->data);
    unix_rights_free(stage->rights);
    if(stage->receiving)task_socket_cancel_receive(task);
    kernel_free(stage);
}

/** @brief Detach and retire held SCM rights on final return or cancellation.
 * @param task Live execution-owned task. The common syscall cleanup still
 * owns the original OFD, and no queue/provider lock spans these releases.
 */
void task_socket_stage_cancel(struct task *task)
{
    struct socket_send_stage *stage=sched_socket_stage_take(task);
    if(!stage)return;
    socket_stage_dispose(task,stage);
}

/** @brief Identify an already accepted receive awaiting bounded output copy.
 * @param task Execution-owned caller. @return True until its saved packet is
 * delivered or cancelled. Caught-signal delivery lets the rewound syscall
 * finish this accepted packet first; fatal/default actions retain priority.
 */
bool task_socket_stage_receive_accepted(const struct task *task)
{
    struct socket_send_stage *stage=task ? sched_socket_stage(task) : NULL;
    return stage && stage->receiving && stage->accepted;
}

/** @brief Reject reuse of an atomic-send stage by a different syscall frame.
 * @param task Current task. @param frame Incoming native register snapshot.
 * A mismatch runs the ordinary cancellation path before new fd admission;
 * sigreturn preserves the interrupted operation's existing cleanup policy.
 */
void task_socket_stage_enter(struct task *task,const struct trap_frame *frame)
{
    if(!task || !frame || frame->rax==LINUX_SYS_RT_SIGRETURN)return;
    struct socket_send_stage *stage=sched_socket_stage(task);
    if(!stage)return;
    const uint64_t args[]={frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8,frame->r9};
    if(stage->number!=frame->rax || __builtin_memcmp(stage->args,args,sizeof(args)))
        task_release_syscall_file(task);
}
#endif

static struct unix_socket *unix_from_file(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) return NULL;
    return (struct unix_socket *)kernel_object_lookup(kernel_objects(),
                                                       (uint32_t)file->aux,
                                                       KERNEL_OBJECT_SOCKET);
}

static struct unix_socket *unix_from_handle(uint32_t handle)
{
    return (struct unix_socket *)kernel_object_lookup(kernel_objects(), handle,
                                                       KERNEL_OBJECT_SOCKET);
}

/**
 * @brief Find a bound AF_UNIX endpoint by abstract or filesystem address.
 * @param path User-visible socket address bytes.
 * @param length Address byte count.
 * @param object Held filesystem inode, NULL for counted abstract names.
 * Serialized execution context; compares actual provider identity without I/O.
 * @return Matching live socket, or null when no endpoint is bound.
 */
static struct unix_socket *unix_find_path(const char *path, uint32_t length, const struct vfs_path *object)
{
    for (struct unix_socket *socket = unix_sockets; socket;
         socket = socket->registry_next) {
        if (!socket || !socket->used || !socket->bound || !socket->path_len) continue;
        if (object && vfs_path_node(object)) {
            if (vfs_path_same_inode(&socket->bound_inode,object)) return socket;
        } else if (!socket->path[0] && socket->path_len == length) {
            uint32_t j;
            for (j = 0; j < length && path[j] == socket->path[j]; ++j) {}
            if (j == length) return socket;
        }
    }
    return NULL;
}

/** @brief Assign a currently unused counted abstract name to an unbound socket.
 * @param socket Borrowed execution-owned endpoint; existing names stay intact.
 * No filesystem lookup, allocation or provider reference is involved.
 */
static void unix_autobind(struct unix_socket *socket)
{
    static uint32_t next_autobind;
    if (socket->path_len) return;
    do {
        uint32_t id = ++next_autobind;
        socket->path[0] = 0;
        for (unsigned i = 0; i < 5; ++i)
            socket->path[i + 1] = "0123456789abcdef"[(id >> (4 * i)) & 15];
    } while (unix_find_path(socket->path, 6, NULL));
    socket->path_len = 6;
    socket->bound = true;
}

static int unix_name_out(const char *path, uint32_t path_len, uint64_t address, uint64_t length)
{
    socklen_t capacity;int prep=socket_user_length(sched_current_task(),length,&capacity);
    if(prep)return prep;
    if ((int32_t)capacity < 0) return -RELIEFOS_EINVAL;
    uint32_t size = 2 + path_len;
    uint32_t copied = capacity < size ? capacity : size;
    prep=socket_user_prepare(sched_current_task(),address,copied,true);if(prep)return prep;
    if (!user_range_writable(address, copied)) return -RELIEFOS_EFAULT;
    unsigned char bytes[111] = {AF_UNIX, 0};
    for (uint32_t i = 0; i < path_len; ++i) bytes[2 + i] = path[i];
    for (uint32_t i = 0; i < copied; ++i) ((unsigned char *)(uintptr_t)address)[i] = bytes[i];
    *(socklen_t *)(uintptr_t)length = size;
    return 0;
}

static int unix_address_out(const struct unix_socket *socket, uint64_t address, uint64_t length)
{
    return unix_name_out(socket ? socket->path : NULL, socket ? socket->path_len : 0, address, length);
}

/**
 * @brief Allocate and register a stable AF_UNIX socket object.
 * @param owner_pid Socket owner process identifier.
 * @param type Socket type.
 * @return New socket or null when either object or handle allocation fails.
 */
static struct unix_socket *unix_alloc(uint32_t owner_pid, uint32_t type)
{
    struct unix_socket *socket = NULL;
    socket = (struct unix_socket *)kernel_malloc(sizeof(*socket));
    if (!socket) return NULL;
    *socket = (struct unix_socket){
        .used = 1, .refs = 1, .owner_pid = owner_pid,
        .state = UNIX_SOCKET_OPEN, .type = type,
        .receive_timeout = UINT64_MAX, .send_timeout = UINT64_MAX,
        .receive_lowwater = 1,
    };
    struct task *owner = sched_find(owner_pid);
    if (owner) socket->credentials = (struct ucred){(int32_t)sched_task_tgid(owner), owner->euid, owner->egid};
    socket->peer_credentials = (struct ucred){0, (uint32_t)-1, (uint32_t)-1};
    socket->handle = kernel_object_insert(kernel_objects(), socket,
                                          KERNEL_OBJECT_SOCKET);
    if (!socket->handle) {
        kernel_free(socket);
        return NULL;
    }
    kernel_wait_queue_init(&socket->wait_rx);
    kernel_wait_queue_init(&socket->wait_tx);
    kernel_wait_queue_init(&socket->wait_accept);
    kernel_wait_queue_init(&socket->wait_connect);
    if (!unix_sockets) unix_sockets = socket;
    else {
        struct unix_socket *tail = unix_sockets;
        while (tail->registry_next) tail = tail->registry_next;
        tail->registry_next = socket;
    }
    return socket;
}

static void unix_wake_peer(struct unix_socket *socket)
{
    struct unix_socket *peer;
    if (!socket || !socket->peer_handle) return;
    peer = unix_from_handle(socket->peer_handle);
    if (!peer) return;
    (void)kernel_wait_queue_wake_all(&peer->wait_rx);
    (void)kernel_wait_queue_wake_all(&peer->wait_tx);
    kernel_poll_readiness_notify();
}

/**
 * @brief Drop one AF_UNIX socket reference and remove its final object safely.
 * @param handle Stable kernel object handle, ignored when already retired.
 * Final teardown releases the bound inode and its provider after removing the
 * endpoint from the registry. Serialized task context; no graph/module lock
 * spans final backend puts or peer wakeups.
 */
static void unix_release_handle(uint32_t handle)
{
    struct unix_socket *socket = unix_from_handle(handle);
    if (!socket) return;
    if (socket->refs) --socket->refs;
    if (socket->refs) return;
    bool unread = socket->rx_head != socket->rx_tail || socket->packets || socket->unaccepted;
    while (socket->cred_head) {
        struct unix_stream_credentials *credentials = socket->cred_head;
        socket->cred_head = credentials->next;
        kernel_free(credentials);
    }
    while (socket->packets) {
        struct unix_packet *p = socket->packets;
        socket->packets = p->next;
        kernel_free(p);
    }
    while (socket->rights) {
        struct unix_rights *rights = socket->rights;
        socket->rights = rights->next;
        unix_rights_free(rights);
    }
    for (uint32_t i = 0; i < socket->pending_count; ++i) unix_release_handle(socket->pending[i]);
    kernel_free(socket->pending);
    if (socket->peer_handle) {
        struct unix_socket *peer = unix_from_handle(socket->peer_handle);
        if (peer) {
            if (socket->type == SOCK_STREAM || socket->type == SOCK_SEQPACKET) {
                peer->shutdown_read = peer->shutdown_write = 1;
                if (unread) peer->error = LINUX_ECONNRESET;
            }
            if (peer->peer_handle == handle) peer->peer_handle = 0;
            (void)kernel_wait_queue_wake_all(&peer->wait_rx);
            (void)kernel_wait_queue_wake_all(&peer->wait_tx);
        }
    }
    (void)kernel_wait_queue_wake_all(&socket->wait_rx);
    (void)kernel_wait_queue_wake_all(&socket->wait_tx);
    (void)kernel_wait_queue_wake_all(&socket->wait_accept);
    (void)kernel_wait_queue_wake_all(&socket->wait_connect);
    kernel_poll_readiness_notify();
    (void)kernel_object_remove(kernel_objects(), handle, KERNEL_OBJECT_SOCKET, NULL);
    struct unix_socket **link = &unix_sockets;
    while (*link && *link != socket) link = &(*link)->registry_next;
    if (*link == socket) *link = socket->registry_next;
    vfs_path_put(&socket->bound_inode);
    kernel_free(socket);
}

static int unix_alloc_fd(struct task *task, struct unix_socket *socket)
{
    if (!task || !socket) return -RELIEFOS_EINVAL;
    struct task_file *file;
    int fd = task_allocate_fd(task, 0, &file);
    if (fd < 0) return fd;
    file->flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_UNIX | RELIEFOS_O_RDWR;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_SOCKET};
    file->aux = socket->handle;
    file->socket_ino = (1ULL << 60) | socket->handle;
    ++socket->refs;
    return fd;
}

static int unix_clone_fd_to_task(struct task *target, struct task_file *source)
{
    if (!target || !source || !source->used) return -RELIEFOS_EBADF;
    struct task_file retained = {0};
    int ret = task_file_reference(&retained, source);
    if (ret < 0) return ret;
    struct task_file *file;
    int fd = task_allocate_fd(target, 0, &file);
    if (fd < 0) { clear_task_file(&retained); return fd; }
    if (file->proc_counted)
        proc_account_file(-1);
    *file = retained;
    return fd;
}

void task_socket_retain(struct task_file *file)
{
    struct unix_socket *socket = unix_from_file(file);
    if (socket) ++socket->refs;
}

void task_socket_release(struct task_file *file)
{
    if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
        unix_release_handle((uint32_t)file->aux);
    }
}

/** @brief Collect unreachable AF_UNIX reference cycles held by queued rights. */
void task_socket_collect(void)
{
    static bool collecting;
    if (collecting) return;
    collecting = true;
    for (struct unix_socket *s = unix_sockets; s; s = s->registry_next) {
        s->gc_roots = s->refs;
        s->gc_mark = 0;
        for (struct unix_rights *r = s->rights; r; r = r->next)
            for (unsigned j = 0; j < r->count; ++j)
                if (r->files[j]) r->files[j]->scm_gc_seen = 0;
    }
    /* References owned only by queued SCM files and pending accepts are
     * graph edges. Everything else is an externally reachable root. */
    for (struct unix_socket *s = unix_sockets; s; s = s->registry_next) {
        for (unsigned j = 0; j < s->pending_count; ++j) {
            struct unix_socket *p = unix_from_handle(s->pending[j]);
            if (p && p->gc_roots) --p->gc_roots;
        }
        for (struct unix_rights *r = s->rights; r; r = r->next) {
            for (unsigned j = 0; j < r->count; ++j) {
                struct task_file *f = r->files[j];
                if (!f) continue;
                struct unix_socket *p = unix_from_file(f);
                if (f->scm_gc_seen) continue;
                f->scm_gc_seen = 1;
                if (p && f->references == f->scm_references && p->gc_roots) --p->gc_roots;
            }
        }
    }
    for (struct unix_socket *s = unix_sockets; s; s = s->registry_next)
        s->gc_mark = s->gc_roots != 0;
    bool changed;
    do {
        changed = false;
        for (struct unix_socket *s = unix_sockets; s; s = s->registry_next) {
            if (!s->gc_mark) continue;
            for (unsigned j = 0; j < s->pending_count; ++j) {
                struct unix_socket *p = unix_from_handle(s->pending[j]);
                if (p && !p->gc_mark) { p->gc_mark = 1; changed = true; }
            }
            for (struct unix_rights *r = s->rights; r; r = r->next)
                for (unsigned j = 0; j < r->count; ++j) {
                    struct unix_socket *p = unix_from_file(r->files[j]);
                    if (p && !p->gc_mark) { p->gc_mark = 1; changed = true; }
                }
        }
    } while (changed);
    struct unix_rights *garbage = NULL;
    for (struct unix_socket *s = unix_sockets; s; s = s->registry_next) {
        if (s->gc_mark) continue;
        while (s->rights) {
            struct unix_rights *r = s->rights;
            s->rights = r->next;
            r->next = garbage;
            garbage = r;
        }
    }
    while (garbage) {
        struct unix_rights *r = garbage;
        garbage = r->next;
        unix_rights_free(r);
    }
    collecting = false;
}

static int unix_read_data(struct task_file *file, void *buffer, uint32_t length,
                          uint32_t flags, struct unix_rights **ancillary,
                          struct unix_receive_result *metadata)
{
    struct unix_socket *socket = unix_from_file(file);
    uint32_t count = 0;
    if (!socket || !(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) return -RELIEFOS_EBADF;
    if (!buffer && length) return -RELIEFOS_EFAULT;
    if (metadata) *metadata = (struct unix_receive_result){.credentials = {0, 65534, 65534}};
    if (socket->type != SOCK_STREAM) {
        kernel_wait_queue_remove(&socket->wait_rx, sched_current_task());
        struct unix_packet *p = socket->packets;
        if (socket->error) {
            int error = socket->error;
            socket->error = 0;
            return -error;
        }
        if (!p) {
            if (socket->shutdown_read) return 0;
            struct unix_socket *peer = unix_from_handle(socket->peer_handle);
            if (socket->type == SOCK_SEQPACKET && (!peer || peer->shutdown_write)) return 0;
            return unix_block(socket, file, &socket->wait_rx, true, flags);
        }
        uint32_t copied = length < p->length ? length : p->length;
        for (unsigned i = 0; i < copied; ++i) ((uint8_t *)buffer)[i] = p->data[i];
        if (metadata) {
            metadata->flags = copied < p->length ? MSG_TRUNC : 0;
            metadata->credentials = p->credentials;
            metadata->path_len = p->path_len;
            for (unsigned i = 0; i < p->path_len; ++i) metadata->path[i] = p->path[i];
        }
        int result = flags & MSG_TRUNC ? (int)p->length : (int)copied;
        struct unix_rights *rights = socket->rights;
        if (rights && rights->position == socket->rx_read) {
            if (ancillary) *ancillary = rights;
            if (!(flags & MSG_PEEK)) {
                socket->rights = rights->next;
                if (!ancillary) unix_rights_free(rights);
            }
        }
        if (!(flags & MSG_PEEK)) {
            socket->packets = p->next;
            if (!socket->packets) socket->packet_tail = NULL;
            --socket->packet_count;
            socket->packet_bytes -= p->length;
            ++socket->rx_read;
            kernel_free(p);
            (void)kernel_wait_queue_wake_all(&socket->wait_tx);
            kernel_poll_readiness_notify();
        }
        return result;
    }
    if (socket->state != UNIX_SOCKET_CONNECTED) return -LINUX_EINVAL;
    if (!length) return 0;
    kernel_wait_queue_remove(&socket->wait_rx, sched_current_task());
    uint32_t tail = socket->rx_tail;
    struct unix_stream_credentials *credentials = socket->cred_head;
    struct ucred received = {0, 65534, 65534};
    if (credentials && credentials->start <= socket->rx_read) received = credentials->credentials;
    if (metadata) metadata->credentials = received;
    if (socket->passcred) {
        struct task *task = sched_current_task();
        if (task->socket_receive_done && socket->rx_tail != socket->rx_head &&
            !unix_credentials_equal(received, (struct ucred){task->socket_receive_pid,
                                      task->socket_receive_uid, task->socket_receive_gid})) return 0;
        if (credentials) {
            uint64_t boundary = credentials->start > socket->rx_read ? credentials->start : credentials->end;
            if (length > boundary - socket->rx_read) length = (uint32_t)(boundary - socket->rx_read);
        }
    }
    struct unix_rights *rights = socket->rights;
    /* Linux consumes preceding ordinary bytes and stops at the end of the
     * sendmsg segment carrying SCM_RIGHTS. Peeking retains this same barrier. */
    if (rights && rights->end > socket->rx_read && length > rights->end - socket->rx_read)
        length = (uint32_t)(rights->end - socket->rx_read);
    while (socket->rx_tail != socket->rx_head && count < length) {
        ((uint8_t *)buffer)[count++] = socket->rx[socket->rx_tail];
        socket->rx_tail = (socket->rx_tail + 1u) % UNIX_SOCKET_RX_CAP;
    }
    if (count) {
        if (rights && rights->position < socket->rx_read + count) {
            if (metadata) metadata->barrier = 1;
            if (ancillary) *ancillary = rights;
            if (!(flags & MSG_PEEK)) {
                socket->rights = rights->next;
                if (!ancillary) unix_rights_free(rights);
            }
        }
        if (flags & MSG_PEEK) socket->rx_tail = tail;
        else {
            socket->rx_read += count;
            while (socket->cred_head && socket->cred_head->end <= socket->rx_read) {
                struct unix_stream_credentials *expired = socket->cred_head;
                socket->cred_head = expired->next;
                kernel_free(expired);
            }
            if (!socket->cred_head) socket->cred_tail = NULL;
        }
        unix_wake_peer(socket);
        return (int)count;
    }
    struct unix_socket *peer = unix_from_handle(socket->peer_handle);
    if (socket->error) {
        int error = socket->error;
        socket->error = 0;
        return -error;
    }
    if (socket->shutdown_read || !peer || peer->shutdown_write) return 0;
    return unix_block(socket, file, &socket->wait_rx, true, flags);
}

uint64_t task_socket_cancel_receive(struct task *task)
{
    uint64_t count = task->socket_receive_done;
    struct task_file *file = task->socket_receive_file;
    task->socket_receive_file = NULL;
    task->socket_receive_done = 0;
    task->socket_receive_pid = 0;
    task->socket_receive_uid = task->socket_receive_gid = 65534;
    task->socket_receive_message = task->socket_receive_control_capacity = 0;
    task->socket_receive_name_capacity = 0;
    task->socket_receive_flags = task->socket_receive_path_length = 0;
    task->socket_receive_name = 0;
    if (file) task_file_put(file);
    return count;
}

static int64_t unix_receive_progress(struct task *task, struct task_file *file,
                                   uint64_t total, int result, uint32_t flags,
                                   const struct unix_receive_result *metadata)
{
    struct unix_socket *socket = unix_from_file(file);
    if (!socket || (flags & MSG_PEEK) || socket->type != SOCK_STREAM)
        return result;
    uint64_t done = task->socket_receive_done + (result > 0 ? (uint32_t)result : 0);
    uint64_t target = flags & MSG_WAITALL ? total : socket->receive_lowwater;
    if (target > total) target = total;
    struct unix_socket *peer = unix_from_handle(socket->peer_handle);
    bool waiting = result == KERNEL_SYSCALL_BLOCKED ||
        (result > 0 && done < target && !metadata->barrier && peer && !peer->shutdown_write &&
         !socket->shutdown_read && !(flags & MSG_DONTWAIT) && !(file->flags & RELIEFOS_O_NONBLOCK));
    if (waiting) {
        if (!task->socket_receive_file) task->socket_receive_file = task_file_get(file);
        if (!task->socket_receive_file) return done ? (int64_t)done : -RELIEFOS_ENOMEM;
        if (!task->socket_receive_done && result > 0) {
            task->socket_receive_pid = metadata->credentials.pid;
            task->socket_receive_uid = metadata->credentials.uid;
            task->socket_receive_gid = metadata->credentials.gid;
        }
        task->socket_receive_done = done;
        if (socket->rx_head == socket->rx_tail) {
            int ret = unix_block(socket, file, &socket->wait_rx, true, flags);
            if (ret != KERNEL_SYSCALL_BLOCKED) {
                task_socket_cancel_receive(task);
                return done ? (int64_t)done : ret;
            }
        }
        return KERNEL_SYSCALL_BLOCKED;
    }
    task_socket_cancel_receive(task);
    return done ? (int64_t)done : result;
}

int task_socket_read(struct task_file *file, void *buffer, uint32_t length)
{
    struct task *task = sched_current_task();
    uint64_t already = task->socket_receive_done;
    if (already > length) { task_socket_cancel_receive(task); return -RELIEFOS_EINVAL; }
    struct unix_receive_result metadata;
    int ret = unix_read_data(file, (uint8_t *)buffer + already, length - already,
                             0, NULL, &metadata);
    return (int)unix_receive_progress(task, file, length, ret, 0, &metadata);
}

/** @brief Find a datagram/packet peer by held inode or counted abstract name.
 * @param socket Borrowed sending endpoint under execution ownership.
 * @param address User sockaddr bytes, NULL to use the connected peer handle.
 * @param length Counted sockaddr bytes when address is non-NULL.
 * @param out Borrowed registry endpoint output, NULL on lookup failure.
 * @return Zero or negative errno; filesystem authorization retains the provider
 * across I/O and releases temporary paths before returning. No locks span I/O.
 */
static int unix_packet_peer(struct unix_socket *socket, const void *address,
                             uint32_t length, struct unix_socket **out)
{
    *out = NULL;
    if (!address) {
        *out = unix_from_handle(socket->peer_handle);
        return *out ? 0 : socket->type == SOCK_DGRAM
            ? (socket->state == UNIX_SOCKET_CONNECTED ? -LINUX_ECONNREFUSED : -LINUX_EDESTADDRREQ)
            : -LINUX_ENOTCONN;
    }
    struct unix_address target={0};
    int result=syscall_unix_address(vfs_system_namespace(),sched_current_task(),
        (uintptr_t)address,length,false,&target);
    if(result)return result;
    if(!target.length)return -RELIEFOS_EINVAL;
    *out=unix_find_path(target.path,target.length,&target.object);
    vfs_path_put(&target.object);
    if (!*out) return -LINUX_ECONNREFUSED;
    if ((*out)->type != socket->type) return -LINUX_EPROTOTYPE;
    return 0;
}

static int unix_send_packet(struct task_file *file, struct unix_socket *peer,
                             const void *data, uint32_t length, uint32_t flags,
                             const struct ucred *credentials)
{
    struct unix_socket *socket = unix_from_file(file);
    if (socket->shutdown_write) return -RELIEFOS_EPIPE;
    if (peer->shutdown_read) return -LINUX_ECONNREFUSED;
    if (socket->type == SOCK_DGRAM && peer->peer_handle && peer->peer_handle != socket->handle)
        return -RELIEFOS_EPERM;
    if (length > 65536) return -LINUX_EMSGSIZE;
    if (socket->passcred) unix_autobind(socket);
    kernel_wait_queue_remove(&peer->wait_tx, sched_current_task());
    if (peer->packet_count >= 10 || peer->packet_bytes > 65536 - length) {
        return unix_block(socket, file, &peer->wait_tx, false, flags);
    }
    struct unix_packet *packet = kernel_malloc(sizeof(*packet) + length);
    if (!packet) return -RELIEFOS_ENOMEM;
    *packet = (struct unix_packet){.length = length, .path_len = socket->path_len};
    packet->credentials = unix_send_credentials(socket, peer, credentials);
    for (unsigned i = 0; i < socket->path_len; ++i) packet->path[i] = socket->path[i];
    for (unsigned i = 0; i < length; ++i) packet->data[i] = ((const uint8_t *)data)[i];
    if (peer->packet_tail) peer->packet_tail->next = packet;
    else peer->packets = packet;
    peer->packet_tail = packet;
    ++peer->packet_count;
    peer->packet_bytes += length;
    ++peer->rx_written;
    (void)kernel_wait_queue_wake_all(&peer->wait_rx);
    kernel_poll_readiness_notify();
    return (int)length;
}

static int unix_write_data(struct task_file *file, const void *buffer, uint32_t length,
                            const struct ucred *credentials)
{
    struct unix_socket *socket = unix_from_file(file);
    struct unix_socket *peer;
    uint32_t count = 0;
    uint32_t free_space;
    if (!socket || !(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) return -RELIEFOS_EBADF;
    if (!buffer && length) return -RELIEFOS_EFAULT;
    if (socket->type != SOCK_STREAM) {
        int result = unix_packet_peer(socket, NULL, 0, &peer);
        return result < 0 ? result : unix_send_packet(file, peer, buffer, length, 0, credentials);
    }
    if (socket->state != UNIX_SOCKET_CONNECTED || socket->shutdown_write) return -RELIEFOS_EPIPE;
    peer = unix_from_handle(socket->peer_handle);
    if (!peer || peer->shutdown_read) return -RELIEFOS_EPIPE;
    kernel_wait_queue_remove(&socket->wait_tx, sched_current_task());
    if (!length) return 0;
    free_space = (peer->rx_tail + UNIX_SOCKET_RX_CAP - peer->rx_head - 1u) %
                 UNIX_SOCKET_RX_CAP;
    if (!free_space) {
        return unix_block(socket, file, &socket->wait_tx, false, 0);
    }
    if (length > free_space) length = free_space;
    struct ucred sent = unix_send_credentials(socket, peer, credentials);
    if (sent.pid) {
        struct unix_stream_credentials *segment = peer->cred_tail;
        if (!segment || segment->end != peer->rx_written ||
            !unix_credentials_equal(segment->credentials, sent)) {
            segment = kernel_malloc(sizeof(*segment));
            if (!segment) return -RELIEFOS_ENOMEM;
            *segment = (struct unix_stream_credentials){.start = peer->rx_written,
                .end = peer->rx_written, .credentials = sent};
            if (peer->cred_tail) peer->cred_tail->next = segment;
            else peer->cred_head = segment;
            peer->cred_tail = segment;
        }
        segment->end += length;
    }
    while (count < length) {
        uint32_t next = (peer->rx_head + 1u) % UNIX_SOCKET_RX_CAP;
        peer->rx[peer->rx_head] = ((const uint8_t *)buffer)[count++];
        peer->rx_head = next;
    }
    peer->rx_written += count;
    (void)kernel_wait_queue_wake_all(&peer->wait_rx);
    kernel_poll_readiness_notify();
    return (int)count;
}

int task_socket_write(struct task_file *file, const void *buffer, uint32_t length)
{
    return unix_write_data(file, buffer, length, NULL);
}

int task_socket_vector(struct task_file *file, const struct iovec *vectors,
                       uint32_t count, uint64_t total, bool writing)
{
    struct unix_socket *socket = unix_from_file(file);
    if (!socket) return -RELIEFOS_EBADF;
    if (writing && socket->type != SOCK_STREAM && total > 65536) return -LINUX_EMSGSIZE;
    struct task *task = sched_current_task();
    uint64_t already = writing ? 0 : task->socket_receive_done;
    if (already > total) { task_socket_cancel_receive(task); return -RELIEFOS_EINVAL; }
    uint32_t limit = socket->type == SOCK_STREAM ? UNIX_SOCKET_RX_CAP : 65536;
    uint32_t length = total - already < limit ? (uint32_t)(total - already) : limit;
    if (!length) return 0;
    uint8_t *buffer = kernel_malloc(length);
    if (!buffer) return -RELIEFOS_ENOMEM;
    uint32_t copied = 0;
    if (writing) {
        for (uint32_t i = 0; i < count && copied < length; ++i) {
            uint64_t n = vectors[i].iov_len;
            if (n > length - copied) n = length - copied;
            for (uint64_t j = 0; j < n; ++j)
                buffer[copied++] = ((const uint8_t *)vectors[i].iov_base)[j];
        }
    }
    /* One vector operation is one packet or one stream receive attempt. */
    struct unix_receive_result metadata;
    int ret = writing ? task_socket_write(file, buffer, length)
                      : unix_read_data(file, buffer, length, 0, NULL, &metadata);
    if (!writing && ret > 0) {
        uint64_t skip = already;
        for (uint32_t i = 0; i < count && copied < (uint32_t)ret; ++i) {
            uint64_t n = vectors[i].iov_len;
            if (skip >= n) { skip -= n; continue; }
            uint64_t offset = skip;
            n -= skip;
            skip = 0;
            if (n > (uint32_t)ret - copied) n = (uint32_t)ret - copied;
            for (uint64_t j = 0; j < n; ++j)
                ((uint8_t *)vectors[i].iov_base)[offset + j] = buffer[copied++];
        }
    }
    kernel_free(buffer);
    if (!writing) ret = (int)unix_receive_progress(task, file, total, ret, 0, &metadata);
    if (ret == -RELIEFOS_EPIPE && writing) sched_signal_user_task(sched_current_pid(), 13);
    return ret;
}

/**
 * @brief Report socket readiness, including errors and shutdown without requiring requested bits.
 * @param file Retained Unix socket open file description.
 * @param events Requested Linux poll bits.
 * @return Requested readiness plus unconditional POLLERR/POLLHUP, or POLLNVAL.
 */
short task_socket_poll(const struct task_file *file, short events)
{
    struct unix_socket *socket = unix_from_file(file);
    short result = 0;
    if (!socket) return POLLNVAL;
    if (socket->error) result |= POLLERR;
    if ((socket->shutdown_read && socket->shutdown_write) ||
        (socket->state == UNIX_SOCKET_OPEN && socket->type != SOCK_DGRAM)) result |= POLLHUP;
    if (socket->shutdown_read) result |= POLLIN | POLLRDNORM | POLLRDHUP;
    bool readable = socket->state == UNIX_SOCKET_LISTEN ? socket->pending_count != 0 :
        socket->type == SOCK_STREAM ? socket->rx_head != socket->rx_tail : socket->packets != NULL;
    if (readable) result |= POLLIN | POLLRDNORM;
    struct unix_socket *peer = unix_from_handle(socket->peer_handle);
    bool writable = !peer || (socket->type == SOCK_STREAM ?
        (peer->rx_head + 1) % UNIX_SOCKET_RX_CAP != peer->rx_tail :
        peer->packet_count < 10 && peer->packet_bytes < 65536);
    if (socket->state != UNIX_SOCKET_LISTEN && writable)
        result |= POLLOUT | POLLWRNORM | POLLWRBAND;
    return result & (events | POLLERR | POLLHUP);
}

/**
 * @brief Returns the receive-generation counter used by edge-triggered epoll.
 * @param file Open socket file description to inspect.
 * @return Monotonic receive generation, or zero for non-UNIX sockets.
 */
uint64_t task_socket_event_generation(const struct task_file *file)
{
    struct unix_socket *socket = unix_from_file(file);
    return socket ? socket->rx_written : 0;
}

/**
 * @brief Query queued receive bytes or update a socket OFD's nonblocking flag.
 * @param file Retained socket open file description.
 * @param request Native FIONREAD or FIONBIO request.
 * @param address User pointer to one 32-bit result or input flag.
 * @return Zero, or negative EBADF/ENOTTY/EINVAL/EFAULT.
 */
int task_socket_ioctl(struct task_file *file, uint64_t request, uint64_t address)
{
    struct unix_socket *socket = unix_from_file(file);
    if (!socket) return -RELIEFOS_EBADF;
    if (request == FIONBIO) {
        int prep=socket_user_prepare(sched_current_task(),address,sizeof(int32_t),false);
        if(prep)return prep;
        if (!user_range_ok(address, sizeof(int32_t))) return -RELIEFOS_EFAULT;
        if (*(const int32_t *)(uintptr_t)address) file->flags |= RELIEFOS_O_NONBLOCK;
        else file->flags &= ~RELIEFOS_O_NONBLOCK;
        return 0;
    }
    if (request != FIONREAD) return -RELIEFOS_ENOTTY;
    if (socket->state == UNIX_SOCKET_LISTEN) return -RELIEFOS_EINVAL;
    int prep=socket_user_prepare(sched_current_task(),address,sizeof(int32_t),true);
    if(prep)return prep;
    if (!user_range_writable(address, sizeof(int32_t))) return -RELIEFOS_EFAULT;
    uint32_t bytes = socket->type == SOCK_STREAM
        ? (socket->rx_head + UNIX_SOCKET_RX_CAP - socket->rx_tail) % UNIX_SOCKET_RX_CAP
        : socket->type == SOCK_SEQPACKET ? socket->packet_bytes
        : socket->packets ? socket->packets->length : 0;
    *(int32_t *)(uintptr_t)address = (int32_t)bytes;
    return 0;
}

/**
 * @brief Receive or peek bytes through the original pinned Internet socket.
 * @param file Pinned descriptor for the socket being read.
 * @param buffer Writable destination for the received bytes.
 * @param length Maximum number of bytes requested.
 * @param flags Linux receive flags; stream PEEK and DONTWAIT are implemented.
 * @return Bytes read, zero for orderly EOF, a negative errno, or the blocking sentinel.
 * @note Unsupported stream flags return EOPNOTSUPP without consuming data.
 * A blocking TCP read retains its syscall-file pin and parks on the socket
 * read queue; the network poll tick retries input because this NIC is polled.
 */
int task_inet_recv(struct task_file *file, void *buffer, uint32_t length, uint32_t flags)
{
    if (file && file->kind == TASK_FILE_KIND_PACKET) return task_packet_recv(file, buffer, length, flags, 0, 0);
    if (file && file->kind == TASK_FILE_KIND_UDP) return task_udp_recv(file, buffer, length, flags, 0, 0);
    struct reliefos_net_socket_io request = {0};
    struct task *task = sched_current_task();
    short ready;
    if (!file || !(file->flags & TASK_FILE_FLAG_SOCKET_INET)) return -RELIEFOS_EBADF;
    if (flags & ~(MSG_PEEK | MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) return -LINUX_EOPNOTSUPP;
    request.socket = (int32_t)file->aux;
    request.buffer = buffer;
    request.length = length;
    if (!length) return 0;
    request.timeout_ms = 1;
    ready = net_socket_poll_fd(request.socket, 0, POLLIN | POLLHUP);
    if (!(ready & (POLLIN | POLLHUP | POLLERR))) {
        if ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -RELIEFOS_EAGAIN;
        if (!task) return -LINUX_ESRCH;
        int wait = net_socket_wait_readable(request.socket);
        if (wait == 1) return KERNEL_SYSCALL_BLOCKED;
        if (wait < 0) return wait;
        ready = net_socket_poll_fd(request.socket, 0, POLLIN | POLLHUP);
    }
    if (ready & POLLERR) { int error = net_socket_error(request.socket, true); if (error) return -error; }
    if ((flags & MSG_PEEK ? net_socket_peek(&request, 0) : net_socket_recv(&request, 0)) < 0) return -RELIEFOS_EIO;
    if (request.status == RELIEFOS_NET_STATUS_OK) return (int)request.transferred;
    if (request.status == RELIEFOS_NET_STATUS_TCP_RESET) return -LINUX_ECONNRESET;
    if (request.status == RELIEFOS_NET_STATUS_SOCKET_CLOSED) return 0;
    if (request.status == RELIEFOS_NET_STATUS_TCP_TIMEOUT &&
        ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT))) return -RELIEFOS_EAGAIN;
    return -RELIEFOS_EIO;
}

#ifdef CONFIG_SWAP
/** @brief Observe one actual INET receive without consuming its queue data.
 * @param file Original pinned socket OFD. @param capacity Requested byte cap.
 * @param flags Native receive flags. @param copied Receives actual copy bound.
 * @return Nonnegative provider result, ordinary errno, or blocked sentinel.
 * Datagram PEEK|TRUNC distinguishes an empty queue from an empty packet. TCP
 * runs its normal readiness/error/EOF path before inspecting queued bytes.
 * No queue pointer survives this call or any subsequent prerequisite wait.
 */
static int inet_receive_probe(struct task_file *file,uint32_t capacity,
                              uint32_t flags,uint32_t *copied)
{
    unsigned char byte=0;
    int result;
    if(file->kind==TASK_FILE_KIND_PACKET)
        result=task_packet_recv_staged(file,&byte,0,flags|MSG_PEEK|MSG_TRUNC,NULL,NULL);
    else if(file->kind==TASK_FILE_KIND_UDP)
        result=task_udp_recv_staged(file,&byte,0,flags|MSG_PEEK|MSG_TRUNC,NULL);
    else {
        result=task_inet_recv(file,&byte,capacity ? 1u : 0u,flags|MSG_PEEK);
        if(result>0) {
            result=net_socket_available((int32_t)file->aux);
            if(result<0)return result;
        }
    }
    if(result<0)return result;
    *copied=(uint32_t)result<capacity ? (uint32_t)result : capacity;
    return result;
}
#endif

/**
 * @brief Read available Internet socket bytes with ordinary read semantics.
 * @param file Original dispatcher-pinned socket description, required.
 * @param buffer Writable kernel/user-validated length-byte destination.
 * @param length Maximum bytes, zero allowed without a buffer access.
 * @return Bytes, zero EOF, negative errno or the blocking sentinel. Retains
 * existing descriptor nonblocking policy without changing the shared OFD.
 */
int task_inet_read(struct task_file *file, void *buffer, uint32_t length)
{
    return task_inet_recv(file, buffer, length, 0);
}

/**
 * @brief Queue bytes to an IPv4 stream socket without waiting while holding the syscall transaction.
 * @param file Open file description for the network socket.
 * @param buffer Validated message bytes to queue.
 * @param length Requested byte count.
 * @return Queued bytes, a negative errno, or KERNEL_SYSCALL_BLOCKED while waiting for queue space.
 * @note Positive short progress returns immediately; data remains owned by the
 * stable TCP socket until ACKed, so no accepted bytes are discarded on retry.
 */
int task_inet_write(struct task_file *file, const void *buffer, uint32_t length)
{
    if (file && file->kind == TASK_FILE_KIND_PACKET) return task_packet_send(file, buffer, length, 0, 0, 0);
    if (file && file->kind == TASK_FILE_KIND_UDP) return task_udp_send(file, buffer, length, 0, 0, 0);
    struct reliefos_net_socket_io request = {0};
    if (!file || !(file->flags & TASK_FILE_FLAG_SOCKET_INET)) return -RELIEFOS_EBADF;
    request.socket = (int32_t)file->aux;
    request.buffer = (void *)buffer;
    request.length = length;
    for (;;) {
        if (net_socket_send(&request, 0) < 0) return -RELIEFOS_EIO;
        if (request.transferred) return (int)request.transferred;
        if (request.status == RELIEFOS_NET_STATUS_OK) return 0;
        if (request.status == RELIEFOS_NET_STATUS_SOCKET_CLOSED ||
            request.status == RELIEFOS_NET_STATUS_TCP_RESET) return -RELIEFOS_EPIPE;
        if (request.status != RELIEFOS_NET_STATUS_TCP_TIMEOUT) return -RELIEFOS_EIO;
        if (file->flags & RELIEFOS_O_NONBLOCK) return -RELIEFOS_EAGAIN;
        int wait = net_socket_wait_writable(request.socket);
        if (wait == 1) return KERNEL_SYSCALL_BLOCKED;
        if (wait < 0) return wait;
    }
}

short task_inet_poll(const struct task_file *file, short events)
{
    if (file && file->kind == TASK_FILE_KIND_PACKET) return task_packet_poll((struct task_file *)file, events);
    if (file && file->kind == TASK_FILE_KIND_UDP) return task_udp_poll((struct task_file *)file, events);
    if (!file || !(file->flags & TASK_FILE_FLAG_SOCKET_INET)) return POLLNVAL;
    return net_socket_poll_fd((int32_t)file->aux, 0, events);
}

void task_inet_retain(struct task_file *file)
{
    (void)file;
}

void task_inet_release(struct task_file *file)
{
    if (file && file->kind == TASK_FILE_KIND_PACKET) { task_packet_release(file); return; }
    if (file && file->kind == TASK_FILE_KIND_UDP) { task_udp_release(file); return; }
    if (file && (file->flags & TASK_FILE_FLAG_SOCKET_INET)) net_socket_release_fd((int32_t)file->aux);
}

/**
 * @brief Read or update supported socket-level options for an IPv4 TCP socket.
 * @param setting Select setsockopt when true, getsockopt otherwise.
 * @param level Socket option level.
 * @param option Option identifier.
 * @param address User option value address.
 * @param length User socklen_t address.
 * @param handle Internal TCP socket handle.
 * @return Zero on success or a negative errno.
 * @note Linux SO_*TIMEO values are converted to scheduler ticks with ceiling
 * rounding. An all-zero timeval maps to an infinite wait; negative seconds map
 * to an immediate timeout and are reported by getsockopt as (0, 0).
 */
static int inet_socket_option(bool setting, int level, int option,
                              uint64_t address, uint64_t length, int32_t handle)
{
    if (setting) {
        if (level != SOL_SOCKET) return -LINUX_ENOPROTOOPT;
        if (option != SO_RCVTIMEO && option != SO_RCVTIMEO_NEW &&
            option != SO_SNDTIMEO && option != SO_SNDTIMEO_NEW)
            return -LINUX_ENOPROTOOPT;
        if ((int32_t)length < (int32_t)sizeof(struct linux_timeval))
            return -LINUX_EINVAL;
        int prep=socket_user_prepare(sched_current_task(),address,sizeof(struct linux_timeval),false);
        if(prep)return prep;
        if (!user_range_ok(address, sizeof(struct linux_timeval)))
            return -LINUX_EFAULT;
        struct linux_timeval timeout = *(const struct linux_timeval *)(uintptr_t)address;
        if (timeout.tv_usec < 0 || timeout.tv_usec >= 1000000)
            return -LINUX_EDOM;
        uint64_t ticks;
        if (timeout.tv_sec < 0) {
            ticks = 0;
        } else if (!timeout.tv_sec && !timeout.tv_usec) {
            ticks = UINT64_MAX;
        } else {
            uint64_t seconds = (uint64_t)timeout.tv_sec;
            uint64_t fraction = ((uint64_t)timeout.tv_usec * RELIEFNT_TICK_HZ + 999999u) /
                                1000000u;
            if (seconds > (UINT64_MAX - 1u - fraction) / RELIEFNT_TICK_HZ)
                ticks = UINT64_MAX - 1u;
            else
                ticks = seconds * RELIEFNT_TICK_HZ + fraction;
        }
        return net_socket_set_timeout_ticks(handle,
            option == SO_SNDTIMEO || option == SO_SNDTIMEO_NEW, ticks);
    }

    socklen_t capacity;int prep=socket_user_length(sched_current_task(),length,&capacity);
    if(prep)return prep;
    if ((int32_t)capacity < 0) return -LINUX_EINVAL;
    if (level != SOL_SOCKET) return -LINUX_ENOPROTOOPT;
    int32_t value = 0;
    struct linux_timeval timeout = {0};
    const void *data = &value;
    uint32_t size = sizeof(value);
    if(option==SO_ERROR) {
        uint32_t copied=capacity<sizeof(value) ? capacity : sizeof(value);
        prep=socket_user_prepare(sched_current_task(),address,copied,true);
        if(prep==KERNEL_SYSCALL_SWAP_CONTENDED)return prep;
    }
    switch (option) {
    case SO_ERROR:
        value = net_socket_error(handle, true);
        break;
    case SO_TYPE:
        value = SOCK_STREAM;
        break;
    case SO_DOMAIN:
        value = AF_INET;
        break;
    case SO_PROTOCOL:
        value = 6;
        break;
    case SO_RCVTIMEO:
    case SO_RCVTIMEO_NEW:
    case SO_SNDTIMEO:
    case SO_SNDTIMEO_NEW: {
        uint64_t ticks = net_socket_get_timeout_ticks(handle,
            option == SO_SNDTIMEO || option == SO_SNDTIMEO_NEW);
        if (ticks != UINT64_MAX) {
            timeout.tv_sec = (int64_t)(ticks / RELIEFNT_TICK_HZ);
            timeout.tv_usec = (int64_t)((ticks % RELIEFNT_TICK_HZ) *
                                        (1000000u / RELIEFNT_TICK_HZ));
        }
        data = &timeout;
        size = sizeof(timeout);
        break;
    }
    default:
        return -LINUX_ENOPROTOOPT;
    }
    if (size > capacity) size = capacity;
    prep=socket_user_prepare(sched_current_task(),address,size,true);if(prep)return prep;
    if (size && !user_range_writable(address, size)) return -LINUX_EFAULT;
    __builtin_memcpy((void *)(uintptr_t)address, data, size);
    *(socklen_t *)(uintptr_t)length = size;
    return 0;
}

/**
 * @brief Dispatch socket operations for IPv4 TCP, UDP, and packet sockets.
 * @param number Linux socket syscall number.
 * @param a0 First syscall argument.
 * @param a1 Second syscall argument.
 * @param a2 Third syscall argument.
 * @param a3 Fourth syscall argument.
 * @param a4 Fifth syscall argument.
 * @return Linux syscall result or a negative errno.
 */
static int64_t inet_socket_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3, uint64_t a4)
{
    struct task *task = sched_current_task();
    struct task_file *file;
    (void)a3;
    if (!task) return -RELIEFOS_EPERM;
    if (number == __NR_socket && ((int)a0 == AF_PACKET || (a1 & 15) == SOCK_RAW))
        return syscall_packet(number, a0, a1, a2, a3, a4, 0);
    if (number == __NR_socket && (a1 & 0xfu) == SOCK_DGRAM)
        return syscall_udp(number, a0, a1, a2, a3, a4, 0);
    if (number != __NR_socket) {
        file = task_file_for_io(task, (int32_t)a0);
        if (file && file->kind == TASK_FILE_KIND_PACKET)
            return syscall_packet(number, a0, a1, a2, a3, a4, 0);
        if (file && file->kind == TASK_FILE_KIND_UDP)
            return syscall_udp(number, a0, a1, a2, a3, a4, 0);
    }
    if (number == __NR_socket) {
        if ((int32_t)a2 && (int32_t)a2 != 6) return -LINUX_EPROTONOSUPPORT;
        if (a1 & ~(uint32_t)(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -RELIEFOS_EINVAL;
        struct reliefos_net_socket_open request = {
            .domain = (uint32_t)a0,
            .type = (uint32_t)a1 & 0xfu,
            .protocol = (uint32_t)a2,
        };
        if ((int)a0 != AF_INET || ((int)a1 & 0x0f) != SOCK_STREAM) {
            return -RELIEFOS_ENOTSUP;
        }
        if (net_socket_open(&request, task->pid, task->uid) < 0) return -RELIEFOS_EIO;
        if (request.status != RELIEFOS_NET_STATUS_OK) return -RELIEFOS_ENOMEM;
        int fd = task_allocate_fd(task, 0, &file);
        if (fd >= 0) {
            file->flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_INET |
                          RELIEFOS_O_RDWR | (((int)a1 & SOCK_NONBLOCK) ? RELIEFOS_O_NONBLOCK : 0u);
            file->fd_flags = ((int)a1 & SOCK_CLOEXEC) ? 1u : 0u;
            file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE};
            file->offset = 0;
            file->aux = (uint64_t)(uint32_t)request.socket;
            file->socket_ino = (2ULL << 60) | (uint32_t)request.socket;
            file->path[0] = 0;
            net_socket_pin_fd(request.socket);
            return fd;
        }
        {
            struct reliefos_net_socket_close close = {.socket = request.socket};
            (void)net_socket_close(&close, task->pid);
        }
        return fd;
    }
    file = task_file_for_io(task, (int)a0);
    if (!file || !(file->flags & TASK_FILE_FLAG_SOCKET_INET)) return -RELIEFOS_EBADF;
    if (number == __NR_connect) {
        struct sockaddr_in address;
        if ((int32_t)a2 < (int32_t)sizeof(address)) return -LINUX_EINVAL;
        int prep=socket_user_prepare(task,a1,sizeof(address),false);if(prep)return prep;
        if (!user_range_ok(a1, sizeof(address))) return -LINUX_EFAULT;
        __builtin_memcpy(&address, (void *)(uintptr_t)a1, sizeof(address));
        if (address.sin_family != AF_INET) return -LINUX_EAFNOSUPPORT;
        return net_socket_connect_fd((int32_t)file->aux, ntohl(address.sin_addr.s_addr),
            ntohs(address.sin_port), (file->flags & RELIEFOS_O_NONBLOCK) != 0);
    }
    if (number == __NR_getsockname || number == __NR_getpeername) {
        uint32_t local_ip, remote_ip;
        uint16_t local_port, remote_port;
        uint32_t length;int prep=socket_user_length(task,a2,&length);if(prep)return prep;
        if ((int32_t)length < 0) return -LINUX_EINVAL;
        if (length > sizeof(struct sockaddr_in)) length = sizeof(struct sockaddr_in);
        prep=socket_user_prepare(task,a1,length,true);if(prep)return prep;
        if (length && !user_range_writable(a1, length)) return -LINUX_EFAULT;
        if (net_socket_address((int32_t)file->aux, 0,
                               &local_ip, &local_port, &remote_ip, &remote_port) < 0) return -RELIEFOS_EBADF;
        if (number == __NR_getpeername && !remote_ip) return -LINUX_ENOTCONN;
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = number == __NR_getsockname ? htons(local_port) : htons(remote_port),
            .sin_addr = {.s_addr = number == __NR_getsockname ? htonl(local_ip) : htonl(remote_ip)},
        };
        __builtin_memcpy((void *)(uintptr_t)a1, &address, length);
        *(uint32_t *)(uintptr_t)a2 = sizeof(address);
        return 0;
    }
    if (number == __NR_shutdown) return net_socket_shutdown_fd((int32_t)file->aux, (int32_t)a1);
    if (number == __NR_setsockopt || number == __NR_getsockopt)
        return inet_socket_option(number == __NR_setsockopt,
                                  (int32_t)a1, (int32_t)a2,
                                  (uint64_t)a3, (uint64_t)a4,
                                  (int32_t)file->aux);
    return -LINUX_EOPNOTSUPP;
}

/** @brief Import native iovec lengths without faulting data pages prematurely. */
static int unix_message_vectors(struct msghdr *message, uint64_t *total)
{
    *total = 0;
    for (uint64_t i = 0; i < message->msg_iovlen; ++i)
        if (message->msg_iov[i].iov_len > INT64_MAX) return -LINUX_EINVAL;
    for (uint64_t i = 0; i < message->msg_iovlen; ++i) {
        struct iovec *v = &message->msg_iov[i];
        /* Linux access_ok checks the architectural range, not page presence. */
        const uint64_t user_limit = (1ULL << 47) - 4096;
        uint64_t address = (uintptr_t)v->iov_base;
        if (address > user_limit || v->iov_len > user_limit - address) return -LINUX_EFAULT;
        if (v->iov_len > 0x7ffff000ULL - *total) v->iov_len = 0x7ffff000ULL - *total;
        *total += v->iov_len;
    }
    return 0;
}

/** @brief Parse one admitted UNIX ancillary buffer and retain each fd entry.
 * @param task Sender. @param message Original message metadata. @param out
 * Receives owned rights. @param credentials Receives supplied credentials.
 * @param explicit_credentials Reports a credentials record. @param kernel_control
 * Selects an already owned kernel snapshot. @return Zero or a negative errno.
 */
static int unix_message_rights_common(struct task *task, const struct msghdr *message,
                                      struct unix_rights **out, struct ucred *credentials,
                                      bool *explicit_credentials,bool kernel_control)
{
    *out = NULL;
    *explicit_credentials = false;
    if (!message->msg_controllen) return 0;
    if (message->msg_controllen > UNIX_SOCKET_CMSG_CAP) return -LINUX_ENOBUFS;
    if (!kernel_control && !user_range_ok((uintptr_t)message->msg_control, message->msg_controllen))
        return -RELIEFOS_EFAULT;
    struct unix_rights *rights = kernel_malloc(sizeof(*rights));
    if (!rights) return -RELIEFOS_ENOMEM;
    *rights = (struct unix_rights){0};
    int result = 0;
    uint64_t position = 0;
    while (position + sizeof(struct cmsghdr) <= message->msg_controllen) {
        struct cmsghdr header;
        const uint8_t *bytes = (const uint8_t *)message->msg_control + position;
        for (unsigned i = 0; i < sizeof(header); ++i) ((uint8_t *)&header)[i] = bytes[i];
        if (header.cmsg_len < CMSG_LEN(0) || header.cmsg_len > message->msg_controllen - position) {
            result = -RELIEFOS_EINVAL; break;
        }
        if (header.cmsg_level != SOL_SOCKET) { position += CMSG_ALIGN(header.cmsg_len); continue; }
        if (header.cmsg_type == SCM_CREDENTIALS) {
            if (header.cmsg_len != CMSG_LEN(sizeof(*credentials))) { result = -RELIEFOS_EINVAL; break; }
            for (unsigned i = 0; i < sizeof(*credentials); ++i)
                ((uint8_t *)credentials)[i] = bytes[CMSG_LEN(0) + i];
            if (credentials->uid == UINT32_MAX || credentials->gid == UINT32_MAX) {
                result = -RELIEFOS_EINVAL; break;
            }
            /* Linux v6.12 net/core/scm.c: each forged field requires its
             * own effective capability, including for an euid-zero sender. */
            uint64_t capabilities = task->cap_effective;
            if ((credentials->pid != (int32_t)sched_task_tgid(task) &&
                 !(capabilities & (1ULL << CAP_SYS_ADMIN))) ||
                (credentials->uid != task->uid && credentials->uid != task->euid &&
                 credentials->uid != task->suid && !(capabilities & (1ULL << CAP_SETUID))) ||
                (credentials->gid != task->gid && credentials->gid != task->egid &&
                 credentials->gid != task->sgid && !(capabilities & (1ULL << CAP_SETGID)))) {
                result = -RELIEFOS_EPERM; break;
            }
            if (credentials->pid <= 0 || !sched_find((uint32_t)credentials->pid)) {
                result = -LINUX_ESRCH; break;
            }
            *explicit_credentials = true;
            position += CMSG_ALIGN(header.cmsg_len);
            continue;
        }
        if (header.cmsg_type != SCM_RIGHTS) {
            result = -RELIEFOS_EINVAL; break;
        }
        uint64_t count = (header.cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (count > UNIX_SOCKET_FD_TRANSFER_MAX - rights->count) { result = -RELIEFOS_EINVAL; break; }
        for (uint64_t i = 0; i < count; ++i) {
            int fd;
            for (unsigned j = 0; j < sizeof(fd); ++j)
                ((uint8_t *)&fd)[j] = bytes[CMSG_LEN(0) + i * sizeof(fd) + j];
            struct task_file *source = task_file_for_fd(task, fd);
            if (!source) {
                result = task_pty_export_fd(task, fd, &rights->ptys[rights->count]);
                if (result < 0) break;
                rights->files[rights->count++] = NULL;
                continue;
            }
            struct task_file *held = task_file_get(source);
            if (!held) { result = -RELIEFOS_ENOMEM; break; }
            ++held->scm_references;
            rights->files[rights->count++] = held;
        }
        if (result) break;
        position += CMSG_ALIGN(header.cmsg_len);
    }
    if (result || !rights->count) unix_rights_free(rights);
    else *out = rights;
    return result;
}

static int unix_message_rights(struct task *task,const struct msghdr *message,
                               struct unix_rights **out,struct ucred *credentials,
                               bool *explicit_credentials)
{
    return unix_message_rights_common(task,message,out,credentials,explicit_credentials,false);
}

#ifdef CONFIG_SWAP
/** @brief Retire send ownership unless an actual retry still owns it.
 * @param task Current owner. @param stage Private stage or NULL.
 * @param rights Locally owned rights when no stage exists. @param result Status.
 * No user pointer is accessed and no queue lock is held.
 */
static void unix_send_finish(struct task *task,struct socket_send_stage *stage,
                             struct unix_rights *rights,int result)
{
    if(!stage)unix_rights_free(rights);
    else if(result!=KERNEL_SYSCALL_BLOCKED && result!=-LINUX_EAGAIN &&
            result!=KERNEL_SYSCALL_SWAP_CONTENDED)
        task_socket_stage_cancel(task);
}
#endif

static int unix_sendmsg(struct task *task, struct task_file *file,
                       struct msghdr message, uint32_t flags,
                       bool batch, uint64_t total, struct socket_message_result *output)
{
    struct unix_socket *socket = unix_from_file(file);
    if (!socket) return -LINUX_ENOTSOCK;
    if (batch) flags |= (uint32_t)message.msg_flags & MSG_EOR;
    if (flags & MSG_OOB) return -LINUX_EOPNOTSUPP;
    int ret;
    output->requested = total;
    if (socket->type != SOCK_STREAM && total > 65536) return -LINUX_EMSGSIZE;
    struct unix_rights *rights = NULL;
    struct ucred credentials={0};
    bool explicit_credentials=false;
#ifdef CONFIG_SWAP
    struct socket_send_stage *stage=sched_socket_stage(task);
    struct task_user_text control={0};
    struct msghdr checked=message;
    if (message.msg_controllen) {
        if (message.msg_controllen>UNIX_SOCKET_CMSG_CAP)return -LINUX_ENOBUFS;
        ret=task_lookup_user_bytes(task,(uintptr_t)message.msg_control,
                                   (uint32_t)message.msg_controllen,&control);
        if(ret)return ret;
        checked.msg_control=(void *)control.text;
    }
    if(stage) {
        if(stage->file!=file || !message.msg_controllen) {
            kernel_free(control.allocation);return -LINUX_EINVAL;
        }
        rights=stage->rights;credentials=stage->credentials;
        explicit_credentials=stage->explicit_credentials;
        ret=0;
    } else if(message.msg_controllen) {
        ret=unix_message_rights_common(task,&checked,&rights,&credentials,&explicit_credentials,true);
        if(!ret && task->lookup_scope.armed && task->syscall_file==file) {
            stage=kernel_malloc(sizeof(*stage));
            if(!stage)ret=-RELIEFOS_ENOMEM;
            else {
                *stage=(struct socket_send_stage){.file=file,.rights=rights,.credentials=credentials,
                    .number=task->lookup_scope.number,.explicit_credentials=explicit_credentials};
                __builtin_memcpy(stage->args,task->lookup_scope.args,sizeof(stage->args));
                if(!sched_socket_stage_set(task,stage)) {
                    kernel_free(stage);stage=NULL;ret=-RELIEFOS_EINVAL;
                } else unix_rights_detach(rights,true);
            }
        }
    } else ret=unix_message_rights(task,&message,&rights,&credentials,&explicit_credentials);
    kernel_free(control.allocation);
    if(ret){unix_rights_free(rights);return ret;}
#else
    ret = unix_message_rights(task, &message, &rights, &credentials, &explicit_credentials);
    if (ret) return ret;
#endif
    uint32_t length = socket->type == SOCK_STREAM && total > UNIX_SOCKET_RX_CAP
        ? UNIX_SOCKET_RX_CAP : (uint32_t)total;
    uint8_t *data = length ? kernel_malloc(length) : NULL;
    if (length && !data) {
#ifdef CONFIG_SWAP
        unix_send_finish(task,stage,rights,-RELIEFOS_ENOMEM);
#else
        unix_rights_free(rights);
#endif
        return -RELIEFOS_ENOMEM;
    }
    uint32_t copied = 0;
    for (uint64_t i = 0; i < message.msg_iovlen && copied < length; ++i) {
        uint64_t n = message.msg_iov[i].iov_len;
        if (n > length - copied) n = length - copied;
#ifdef CONFIG_SWAP
        uint64_t offset=0;
        while(offset<n) {
            uint32_t chunk=n-offset>32768u ? 32768u : (uint32_t)(n-offset);
            struct task_user_text part={0};
            ret=task_lookup_user_bytes(task,(uintptr_t)message.msg_iov[i].iov_base+offset,chunk,&part);
            if(ret) {
                kernel_free(data);unix_send_finish(task,stage,rights,ret);return ret;
            }
            __builtin_memcpy(data+copied,part.text,chunk);
            kernel_free(part.allocation);copied+=chunk;offset+=chunk;
        }
#else
        if (n && !user_range_ok((uintptr_t)message.msg_iov[i].iov_base, n)) {
            kernel_free(data);
            unix_rights_free(rights);
            return -LINUX_EFAULT;
        }
        for (uint64_t j = 0; j < n; ++j) data[copied++] = ((uint8_t *)message.msg_iov[i].iov_base)[j];
#endif
    }
    struct unix_socket *peer = unix_from_handle(socket->peer_handle);
    if (socket->type != SOCK_STREAM) {
        ret = unix_packet_peer(socket, message.msg_name, message.msg_namelen, &peer);
        if (ret < 0) {
            kernel_free(data);
#ifdef CONFIG_SWAP
            unix_send_finish(task,stage,rights,ret);
#else
            unix_rights_free(rights);
#endif
            return ret;
        }
    } else if (message.msg_name) {
        kernel_free(data);
#ifdef CONFIG_SWAP
        unix_send_finish(task,stage,rights,-RELIEFOS_EISCONN);
#else
        unix_rights_free(rights);
#endif
        return -RELIEFOS_EISCONN;
    }
    uint64_t position = peer ? peer->rx_written : 0;
    struct task_file local = *file;
    if (flags & MSG_DONTWAIT) local.flags |= RELIEFOS_O_NONBLOCK;
    ret = socket->type == SOCK_STREAM ? unix_write_data(&local, data ? data : (uint8_t *)"", length,
            explicit_credentials ? &credentials : NULL)
        : unix_send_packet(&local, peer, data, length, flags, explicit_credentials ? &credentials : NULL);
    if (data) kernel_free(data);
    if ((ret > 0 || (ret == 0 && socket->type != SOCK_STREAM)) && rights && peer) {
#ifdef CONFIG_SWAP
        unix_rights_detach(rights,false);
        if(stage)stage->rights=NULL;
#endif
        rights->position = position;
        rights->end = socket->type == SOCK_STREAM ? position + (uint32_t)ret : position + 1;
        struct unix_rights **tail = &peer->rights;
        while (*tail) tail = &(*tail)->next;
        *tail = rights;
        rights=NULL;
    }
#ifdef CONFIG_SWAP
    unix_send_finish(task,stage,rights,ret);
#else
    unix_rights_free(rights);
#endif
    if (ret == -RELIEFOS_EPIPE && !(flags & MSG_NOSIGNAL)) sched_signal_user_task(task->pid, 13);
    return ret;
}

#ifdef CONFIG_SWAP
/** @brief Send a scalar UNIX socket write using bounded retry-stable input.
 * @param task Calling task under execution ownership. @param file Admitted
 * socket description. @param address User buffer. @param length Capped Linux
 * write count. @return Accepted bytes, errno, or an internal retry marker.
 * A datagram is imported in bounded pieces and published once; a stream uses
 * its existing bounded send limit. Matching frame retries retain snapshots in
 * the task lookup scope; common cancellation retires them and the held OFD.
 */
int task_socket_send_user(struct task *task,struct task_file *file,uint64_t address,uint32_t length)
{
    struct iovec vector={.iov_base=(void *)(uintptr_t)address,.iov_len=length};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1};
    struct socket_message_result output={0};
    return unix_sendmsg(task,file,message,0,false,length,&output);
}
#endif

static uint64_t unix_put_credentials(struct msghdr *message, uint64_t capacity,
                                      const struct ucred *credentials)
{
    uint64_t length = CMSG_LEN(sizeof(*credentials));
    if (capacity < length) message->msg_flags |= MSG_CTRUNC;
    if (capacity < CMSG_LEN(0)) return 0;
    if (length > capacity) length = capacity;
    struct cmsghdr header = {.cmsg_len = length, .cmsg_level = SOL_SOCKET,
                            .cmsg_type = SCM_CREDENTIALS};
    uint8_t *out = message->msg_control;
    for (unsigned i = 0; i < sizeof(header); ++i) out[i] = ((uint8_t *)&header)[i];
    for (unsigned i = 0; i < length - CMSG_LEN(0); ++i)
        out[CMSG_LEN(0) + i] = ((const uint8_t *)credentials)[i];
    uint64_t used = CMSG_SPACE(sizeof(*credentials));
    return used < capacity ? used : capacity;
}

#ifdef CONFIG_SWAP
/** @brief Acquire independent ownership of one observed PEEK ancillary list.
 * @param source Borrowed list while execution owns the queue. @param out
 * Required independent list output. Each repeated OFD/PTY entry gets its own
 * reference. @return Zero or negative allocation/invalid-reference errno.
 */
static int unix_peek_rights(const struct unix_rights *source,struct unix_rights **out)
{
    *out=NULL;if(!source)return 0;
    struct unix_rights *copy=kernel_malloc(sizeof(*copy));
    if(!copy)return -RELIEFOS_ENOMEM;
    *copy=(struct unix_rights){.position=source->position,.end=source->end,.detached=true};
    for(uint32_t i=0;i<source->count;++i) {
        if(source->files[i]) {
            struct task_file *file=task_file_get(source->files[i]);
            if(!file){unix_rights_free(copy);return -RELIEFOS_ENOMEM;}
            copy->files[copy->count++]=file;
        } else {
            struct task_pty_fd pty=source->ptys[i];
            if(!pty.used || !pty.description || pty.description->references==UINT32_MAX) {
                unix_rights_free(copy);return -RELIEFOS_EBADF;
            }
            ++pty.description->references;copy->ptys[copy->count++]=pty;
        }
    }
    *out=copy;return 0;
}

/** @brief Pin one bounded metadata output a page at a time.
 * @param task Execution-owned task. @param stage Owned receive stage.
 * @param index Region index. @return Zero, EFAULT, or private contention.
 * Numeric user addresses are revalidated before each pin; across a retry the
 * stage retains physical/cache ownership, never a borrowed user mapping.
 */
static int socket_pin_output(struct task *task,struct socket_send_stage *stage,uint32_t index)
{
    struct socket_output_region *region=&stage->regions[index];
    while(region->pinned<region->length) {
        uint64_t address=region->address+region->pinned;
        if(address<region->address)return -LINUX_EFAULT;
        uint32_t offset=(uint32_t)(address&4095u),length=4096u-offset;
        if(length>region->length-region->pinned)length=region->length-region->pinned;
        int ret=swap_prepare_user_range(task,address,length,true);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret<0 || !user_range_writable(address,length))return -LINUX_EFAULT;
        uint64_t physical=address_space_user_page_phys(sched_task_as(task),address);
        uint64_t flags=address_space_user_page_flags(sched_task_as(task),address);
        if(!physical || stage->pin_count==SOCKET_OUTPUT_PINS)return -LINUX_EFAULT;
        if(flags&RELIEFNT_PAGE_DEVICE) {
            struct device_user_page device_page={0};
            int error=device_user_page_get(task,address,physical,flags,&device_page);
            if(error)return error;
            struct socket_output_pin *pin=&stage->pins[stage->pin_count++];
            *pin=(struct socket_output_pin){.physical=physical,.offset=offset,.length=length,
                .region=index,.device=true,.device_page=device_page};
            region->pinned+=length;continue;
        }
        if(!mm_phys_to_virt(physical))return -LINUX_EFAULT;
        bool cache=page_cache_retain(physical)==0;
        if(!cache) {
            if(!mm_page_refcount(physical))return -LINUX_EFAULT;
            mm_retain_page(physical);
        } else page_cache_mark_shared_writable(physical);
        struct socket_output_pin *pin=&stage->pins[stage->pin_count++];
        *pin=(struct socket_output_pin){.physical=physical,.offset=offset,.length=length,
                                       .region=index,.cache=cache};
        paging_writable_alias_add(&pin->alias,physical);
        region->pinned+=length;
    }
    return 0;
}

/** @brief Copy into a fully pinned output region without dereferencing its VA.
 * @param stage Owning stage. @param index Region. @param offset Byte offset.
 * @param source Kernel bytes. @param length Count within the retained region.
 */
static void socket_output_copy(const struct socket_send_stage *stage,uint32_t index,
                               uint32_t offset,const void *source,uint32_t length)
{
    uint32_t position=0,end=offset+length;
    for(uint32_t i=0;i<stage->pin_count && position<end;++i) {
        const struct socket_output_pin *pin=&stage->pins[i];
        if(pin->region!=index)continue;
        uint32_t limit=position+pin->length;
        if(limit>offset && position<end) {
            uint32_t first=position>offset?position:offset,last=limit<end?limit:end;
            uint8_t *alias=pin->device ? pin->device_page.alias : mm_phys_to_virt(pin->physical);
            __builtin_memcpy(alias+pin->offset+first-position,
                             (const uint8_t *)source+first-offset,last-first);
            if (!pin->device && page_cache_note_write) page_cache_note_write(pin->physical);
        }
        position=limit;
    }
}

/** @brief Retire one receive stage, optionally retaining WAITALL progress.
 * @param task Owner. @param stage Private stage. @param scoped True if linked.
 * @param keep_receive Existing WAITALL state remains owned by the task.
 */
static void socket_receive_finish(struct task *task,struct socket_send_stage *stage,
                                  bool scoped,bool keep_receive)
{
    if(keep_receive)stage->receiving=false;
    if(scoped)task_socket_stage_cancel(task);
    else socket_stage_dispose(task,stage);
}

/** @brief Receive one UNIX message into bounded resumable owned staging.
 * @param task Live caller. @param file Admitted socket OFD. @param user_msg
 * Original numeric msghdr address, or zero for scalar/vector operations.
 * @param name_length_pointer Optional recvfrom socklen output. @param message
 * Imported header and vector snapshot. @param flags Receive flags. @param total Checked vector capacity.
 * @param output Message result. @return Count, errno or private wait marker.
 * Accepted packet bytes and ancillary ownership survive contention; each
 * output data slice is prepared and copied once. Metadata/descriptor outputs
 * are pinned before descriptor installation so retries cannot duplicate it.
 */
static int unix_recvmsg_staged(struct task *task,struct task_file *file,uint64_t user_msg,
                               uint64_t name_length_pointer,struct msghdr message,uint32_t flags,uint64_t total,
                               struct socket_message_result *output)
{
    if(flags & MSG_OOB)return -LINUX_EOPNOTSUPP;
    struct unix_socket *socket=unix_from_file(file);
    if(!socket)return -LINUX_ENOTSOCK;
    struct socket_send_stage *stage=sched_socket_stage(task);
    bool scoped=stage!=NULL;
    if(stage && (!stage->receiving || stage->file!=file))return -LINUX_EINVAL;
    if(!stage) {
        if(task->socket_receive_done>total){task_socket_cancel_receive(task);return -LINUX_EINVAL;}
        uint32_t maximum=socket->type==SOCK_STREAM?UNIX_SOCKET_RX_CAP:65536u;
        uint64_t remaining=total-task->socket_receive_done;
        uint32_t capacity=remaining>maximum?maximum:(uint32_t)remaining;
        stage=kernel_malloc(sizeof(*stage));if(!stage)return -RELIEFOS_ENOMEM;
        *stage=(struct socket_send_stage){.file=file,.receiving=true,.prior_done=task->socket_receive_done,
            .number=task->lookup_scope.number,.receive_header=user_msg,.name_address=(uintptr_t)message.msg_name,
            .name_length_pointer=name_length_pointer,.name_capacity=message.msg_namelen};
        __builtin_memcpy(stage->args,task->lookup_scope.args,sizeof(stage->args));
        if(capacity) {
            stage->data=kernel_malloc(capacity);
            if(!stage->data){kernel_free(stage);return -RELIEFOS_ENOMEM;}
        }
        if(task->lookup_scope.armed && task->syscall_file==file) {
            if(!sched_socket_stage_set(task,stage)){socket_stage_dispose(task,stage);return -LINUX_EINVAL;}
            scoped=true;
        }
    }
    if(!stage->accepted) {
        uint32_t maximum=socket->type==SOCK_STREAM?UNIX_SOCKET_RX_CAP:65536u;
        uint64_t remaining=total-stage->prior_done;
        uint32_t capacity=remaining>maximum?maximum:(uint32_t)remaining;
        struct unix_rights *rights=NULL;
        int ret=unix_read_data(file,stage->data?stage->data:(uint8_t *)"",capacity,flags,&rights,&stage->metadata);
        if(stage->prior_done)stage->metadata.credentials=(struct ucred){task->socket_receive_pid,
                                                      task->socket_receive_uid,task->socket_receive_gid};
        if(ret<0) {
            int progress=(int)unix_receive_progress(task,file,total,ret,flags,&stage->metadata);
            if(progress==KERNEL_SYSCALL_BLOCKED) {
                if(!scoped)socket_receive_finish(task,stage,false,true);
                return progress;
            }
            if(progress<0){socket_receive_finish(task,stage,scoped,false);return progress;}
            stage->receive_result=progress;stage->data_length=0;stage->progress_done=true;
        } else {
            if(flags & MSG_PEEK) {
                int clone=unix_peek_rights(rights,&stage->rights);
                if(clone){socket_receive_finish(task,stage,scoped,false);return clone;}
            } else {stage->rights=rights;if(rights)rights->next=NULL;unix_rights_detach(rights,true);}
            stage->receive_result=ret;
            stage->data_length=(uint32_t)ret<capacity?(uint32_t)ret:capacity;
        }
        stage->passcred=socket->passcred;stage->accepted=true;
    }
    uint64_t skip=stage->prior_done+stage->copied;
    for(uint64_t i=0;i<message.msg_iovlen && stage->copied<stage->data_length;++i) {
        uint64_t length=message.msg_iov[i].iov_len;
        if(skip>=length){skip-=length;continue;}
        uint64_t offset=skip;skip=0;length-=offset;
        if(length>stage->data_length-stage->copied)length=stage->data_length-stage->copied;
        while(length) {
            uint32_t chunk=length>32768u?32768u:(uint32_t)length;
            uint64_t address=(uintptr_t)message.msg_iov[i].iov_base+offset;
            if(address<(uintptr_t)message.msg_iov[i].iov_base) {
                socket_receive_finish(task,stage,scoped,false);return -LINUX_EFAULT;
            }
            int ret=swap_prepare_user_range(task,address,chunk,true);
            if(ret==SWAP_STORAGE_CONTENDED) {
                if(!scoped)socket_receive_finish(task,stage,false,false);
                return KERNEL_SYSCALL_SWAP_CONTENDED;
            }
            if(ret<0 || !user_range_writable(address,chunk)) {
                int progress=(int)(stage->prior_done+stage->copied);
                socket_receive_finish(task,stage,scoped,false);return progress?progress:-LINUX_EFAULT;
            }
            __builtin_memcpy((void *)(uintptr_t)address,stage->data+stage->copied,chunk);
            stage->copied+=chunk;offset+=chunk;length-=chunk;
        }
    }
    int ret=stage->receive_result;
    if(!stage->progress_done) {
        ret=(int)unix_receive_progress(task,file,total,ret,flags,&stage->metadata);
        if(ret==KERNEL_SYSCALL_BLOCKED) {
            task->socket_receive_message=user_msg;
            task->socket_receive_control_capacity=message.msg_controllen;
            task->socket_receive_name_capacity=message.msg_namelen;
            task->socket_receive_name=(uintptr_t)message.msg_name;
            if(!stage->prior_done && stage->metadata.path_len) {
                task->socket_receive_path_length=stage->metadata.path_len+2;
                task->socket_receive_path[0]=AF_UNIX;task->socket_receive_path[1]=0;
                __builtin_memcpy(task->socket_receive_path+2,stage->metadata.path,stage->metadata.path_len);
            }
            task->socket_receive_flags=stage->metadata.flags | (flags & MSG_CMSG_CLOEXEC);
            if(scoped) {
                stage->accepted=false;stage->progress_done=false;stage->prior_done=task->socket_receive_done;
                stage->data_length=stage->copied=0;
            } else socket_receive_finish(task,stage,false,true);
            return ret;
        }
        stage->receive_result=ret;stage->progress_done=true;
    }
    uint64_t capacity=message.msg_controllen;
    uint32_t name_length=stage->metadata.path_len?stage->metadata.path_len+2:0;
    uint32_t name_bytes=message.msg_name?(message.msg_namelen<name_length?message.msg_namelen:name_length):0;
    uint32_t credential_bytes=0,credential_used=0;
    if(stage->passcred && capacity>=CMSG_LEN(0)) {
        credential_bytes=(uint32_t)(capacity<CMSG_LEN(sizeof(struct ucred))?capacity:CMSG_LEN(sizeof(struct ucred)));
        credential_used=(uint32_t)(capacity<CMSG_SPACE(sizeof(struct ucred))?capacity:CMSG_SPACE(sizeof(struct ucred)));
    }
    uint64_t remaining_control=capacity-credential_used;
    uint64_t rights_limit=remaining_control>=CMSG_LEN(0)?(remaining_control-CMSG_LEN(0))/sizeof(int):0;
    uint32_t possible=stage->rights && rights_limit<stage->rights->count?(uint32_t)rights_limit:
                      stage->rights?stage->rights->count:0;
    uint32_t control_bytes=credential_bytes;
    if(possible && credential_used+CMSG_LEN(possible*sizeof(int))>control_bytes)
        control_bytes=credential_used+CMSG_LEN(possible*sizeof(int));
    if(!stage->outputs_planned) {
        if(user_msg && user_msg>UINT64_MAX-sizeof(struct msghdr)) {
            socket_receive_finish(task,stage,scoped,false);return -LINUX_EFAULT;
        }
        stage->regions[0]=(struct socket_output_region){(uintptr_t)message.msg_name,name_bytes,0};
        stage->regions[1]=(struct socket_output_region){(uintptr_t)message.msg_control,control_bytes,0};
        stage->regions[2]=(struct socket_output_region){user_msg?user_msg+__builtin_offsetof(struct msghdr,msg_namelen):name_length_pointer,message.msg_name?sizeof(message.msg_namelen):0,0};
        stage->regions[3]=(struct socket_output_region){user_msg?user_msg+__builtin_offsetof(struct msghdr,msg_flags):0,user_msg?sizeof(message.msg_flags):0,0};
        stage->regions[4]=(struct socket_output_region){user_msg?user_msg+__builtin_offsetof(struct msghdr,msg_controllen):0,user_msg?sizeof(message.msg_controllen):0,0};
        stage->outputs_planned=true;
    }
    for(uint32_t i=0;i<SOCKET_OUTPUT_REGIONS;++i) {
        int pin=socket_pin_output(task,stage,i);
        if(pin) {
            if(pin!=KERNEL_SYSCALL_SWAP_CONTENDED || !scoped)socket_receive_finish(task,stage,scoped,false);
            return pin;
        }
    }
    unsigned char name[111]={AF_UNIX,0};
    if(stage->metadata.path_len)__builtin_memcpy(name+2,stage->metadata.path,stage->metadata.path_len);
    if(name_bytes)socket_output_copy(stage,0,0,name,name_bytes);
    unsigned char control[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(UNIX_SOCKET_FD_TRANSFER_MAX*sizeof(int))]={0};
    struct msghdr prepared=message;prepared.msg_control=control;prepared.msg_controllen=0;
    prepared.msg_flags=stage->metadata.flags | (flags & MSG_CMSG_CLOEXEC);
    if(stage->passcred)prepared.msg_controllen=unix_put_credentials(&prepared,capacity,&stage->metadata.credentials);
    if(credential_bytes)socket_output_copy(stage,1,0,control,credential_bytes);
    uint32_t count=0;
    while(stage->rights && count<possible) {
        int fd=stage->rights->files[count]?unix_clone_fd_to_task(task,stage->rights->files[count]):
            task_pty_import_fd(task,&stage->rights->ptys[count],flags & MSG_CMSG_CLOEXEC?RELIEFOS_FD_CLOEXEC:0);
        if(fd<0)break;
        if((flags & MSG_CMSG_CLOEXEC) && stage->rights->files[count])task_descriptor_for_fd(task,fd)->fd_flags=RELIEFOS_FD_CLOEXEC;
        __builtin_memcpy(control+credential_used+CMSG_LEN(0)+count*sizeof(int),&fd,sizeof(fd));++count;
    }
    if(count) {
        struct cmsghdr header={.cmsg_len=CMSG_LEN(count*sizeof(int)),.cmsg_level=SOL_SOCKET,.cmsg_type=SCM_RIGHTS};
        __builtin_memcpy(control+credential_used,&header,sizeof(header));
        socket_output_copy(stage,1,credential_used,control+credential_used,CMSG_LEN(count*sizeof(int)));
        uint64_t size=CMSG_SPACE(count*sizeof(int));
        prepared.msg_controllen+=size<remaining_control?size:remaining_control;
    }
    if(stage->rights && count<stage->rights->count)prepared.msg_flags|=MSG_CTRUNC;
    if(message.msg_name)socket_output_copy(stage,2,0,&name_length,sizeof(name_length));
    if(user_msg) {
        socket_output_copy(stage,3,0,&prepared.msg_flags,sizeof(prepared.msg_flags));
        socket_output_copy(stage,4,0,&prepared.msg_controllen,sizeof(prepared.msg_controllen));
    }
    output->flags=(uint32_t)prepared.msg_flags;
    socket_receive_finish(task,stage,scoped,false);return ret;
}

/** @brief Receive a scalar UNIX buffer or recvfrom result through the stage.
 * @param task Current caller. @param file Admitted socket. @param address User
 * payload address. @param length Declared payload length. @param flags Native
 * receive flags. @param name Optional sockaddr output. @param name_length_pointer
 * User socklen address when name is nonzero. @return Count, errno or retry.
 */
int task_socket_receive_user(struct task *task,struct task_file *file,uint64_t address,uint64_t length,
                             uint32_t flags,uint64_t name,uint64_t name_length_pointer)
{
    if(flags & ~(MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC|MSG_WAITALL|MSG_CMSG_CLOEXEC))return -LINUX_EOPNOTSUPP;
    if(length && (address<RELIEFNT_USER_BASE || address>=RELIEFNT_USER_TOP ||
                  length>RELIEFNT_USER_TOP-address))return -LINUX_EFAULT;
    uint32_t capacity=0;
    if(name) {
        struct task_user_text imported={0};
        int ret=task_lookup_user_bytes(task,name_length_pointer,sizeof(capacity),&imported);
        if(ret)return ret;
        __builtin_memcpy(&capacity,imported.text,sizeof(capacity));kernel_free(imported.allocation);
        if((int32_t)capacity<0)return -LINUX_EINVAL;
        if(capacity>111)capacity=111;
    }
    struct iovec vector={.iov_base=(void *)(uintptr_t)address,.iov_len=length};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_name=(void *)(uintptr_t)name,.msg_namelen=capacity};
    struct socket_message_result result={0};
    return unix_recvmsg_staged(task,file,0,name_length_pointer,message,flags,length,&result);
}

/** @brief Perform one UNIX vector transfer from a retry-stable iovec snapshot.
 * @param task Current caller. @param file Admitted UNIX socket. @param address
 * Native iovec array address. @param count Previously validated UIO_MAXIOV count.
 * @param writing True for writev. @return Count, errno or private retry marker.
 * Payloads are copied in bounded slices; a datagram is committed exactly once.
 */
int task_socket_vector_user(struct task *task,struct task_file *file,uint64_t address,uint32_t count,bool writing)
{
    if(count>1024)return -LINUX_EINVAL;
    struct task_user_text imported={0};
    int ret=task_lookup_user_bytes(task,address,count*sizeof(struct iovec),&imported);
    if(ret)return ret;
    struct iovec vectors[1024];uint64_t total=0;
    __builtin_memcpy(vectors,imported.text,count*sizeof(struct iovec));kernel_free(imported.allocation);
    for(uint32_t i=0;i<count;++i) {
        if((int64_t)vectors[i].iov_len<0)return -LINUX_EINVAL;
        if(vectors[i].iov_len>0x7ffff000u-total)vectors[i].iov_len=0x7ffff000u-total;
        uint64_t base=(uintptr_t)vectors[i].iov_base,length=vectors[i].iov_len;
        if(length && (base<RELIEFNT_USER_BASE || base>=RELIEFNT_USER_TOP || length>RELIEFNT_USER_TOP-base))return -LINUX_EFAULT;
        total+=length;
    }
    if(!total)return 0;
    struct msghdr message={.msg_iov=vectors,.msg_iovlen=count};
    struct socket_message_result result={0};
    return writing?unix_sendmsg(task,file,message,0,false,total,&result):
                   unix_recvmsg_staged(task,file,0,0,message,0,total,&result);
}

/** @brief Prepare interrupted WAITALL metadata at the outer signal boundary.
 * @param task Live execution-owned receive owner. @return Zero or transient
 * storage contention. Invalid outputs retain their existing late EFAULT path.
 */
int task_socket_signal_prepare(struct task *task)
{
    struct socket_send_stage *stage=task?sched_socket_stage(task):NULL;
    if(!stage || !stage->receiving || stage->accepted || !task->socket_receive_done)return 0;
    uint64_t addresses[4]={0},lengths[4]={0};uint32_t count=0;
    if(stage->name_address) {
        uint32_t length=task->socket_receive_path_length;
        if(length>stage->name_capacity)length=stage->name_capacity;
        if(length){addresses[count]=stage->name_address;lengths[count++]=length;}
        addresses[count]=stage->receive_header?stage->receive_header+__builtin_offsetof(struct msghdr,msg_namelen):stage->name_length_pointer;
        lengths[count++]=sizeof(uint32_t);
    }
    if(stage->receive_header) {
        addresses[count]=stage->receive_header+__builtin_offsetof(struct msghdr,msg_flags);lengths[count++]=sizeof(uint32_t);
        addresses[count]=stage->receive_header+__builtin_offsetof(struct msghdr,msg_controllen);lengths[count++]=sizeof(uint64_t);
    }
    for(uint32_t i=0;i<count;++i) {
        int ret=swap_prepare_user_range(task,addresses[i],lengths[i],true);
        if(ret==SWAP_STORAGE_CONTENDED)return ret;
    }
    return 0;
}

/** @brief Publish non-batch interrupted WAITALL metadata once before cleanup.
 * @param task Live execution-owned receive owner. @return Zero or late EFAULT.
 */
int task_socket_signal_finish(struct task *task)
{
    struct socket_send_stage *stage=task?sched_socket_stage(task):NULL;
    if(!stage || !stage->receiving || stage->accepted || !task->socket_receive_done)return 0;
    if(stage->name_address) {
        uint32_t length=task->socket_receive_path_length,n=length;
        if(n>stage->name_capacity)n=stage->name_capacity;
        uint64_t length_pointer=stage->receive_header?stage->receive_header+__builtin_offsetof(struct msghdr,msg_namelen):stage->name_length_pointer;
        if((n && user_copy_to_task(task,stage->name_address,task->socket_receive_path,n)<0) ||
           user_copy_to_task(task,length_pointer,&length,sizeof(length))<0)return -LINUX_EFAULT;
    }
    if(stage->receive_header) {
        uint32_t flags=task->socket_receive_flags;uint64_t no_control=0;
        if(user_copy_to_task(task,stage->receive_header+__builtin_offsetof(struct msghdr,msg_flags),&flags,sizeof(flags))<0 ||
           user_copy_to_task(task,stage->receive_header+__builtin_offsetof(struct msghdr,msg_controllen),&no_control,sizeof(no_control))<0)
            return -LINUX_EFAULT;
    }
    return 0;
}
#endif

#ifndef CONFIG_SWAP
static int unix_recvmsg(struct task *task, struct task_file *file,
                        const struct msghdr *user_msg, struct msghdr message,
                        uint32_t flags, uint64_t total, struct socket_message_result *output)
{
    if (flags & MSG_OOB) return -LINUX_EOPNOTSUPP;
    if (task->socket_receive_message == (uintptr_t)user_msg && task->socket_receive_file) {
        message.msg_controllen = task->socket_receive_control_capacity;
        message.msg_namelen = task->socket_receive_name_capacity;
    }
    int ret;
    uint64_t already = task->socket_receive_done;
    if (already > total) { task_socket_cancel_receive(task); return -RELIEFOS_EINVAL; }
    if (message.msg_controllen && !user_range_writable((uintptr_t)message.msg_control, message.msg_controllen))
        return -RELIEFOS_EFAULT;
    if (message.msg_name && message.msg_namelen &&
        !user_range_writable((uintptr_t)message.msg_name, message.msg_namelen)) return -RELIEFOS_EFAULT;
    struct unix_socket *socket = unix_from_file(file);
    if (socket && socket->type == SOCK_STREAM) {
        /* The ring backend commits stream consumption in unix_read_data. */
        for (uint64_t i = 0; i < message.msg_iovlen; ++i)
            if (message.msg_iov[i].iov_len &&
                !user_range_writable((uintptr_t)message.msg_iov[i].iov_base, message.msg_iov[i].iov_len))
                return -LINUX_EFAULT;
    }
    uint32_t maximum = socket && socket->type != SOCK_STREAM ? 65536 : UNIX_SOCKET_RX_CAP;
    bool passcred = socket && socket->passcred;
    uint64_t remaining = total - already;
    uint32_t length = remaining > maximum ? maximum : (uint32_t)remaining;
    uint8_t *data = length ? kernel_malloc(length) : NULL;
    if (length && !data) return -RELIEFOS_ENOMEM;
    struct unix_rights *rights = NULL;
    struct unix_receive_result metadata;
    ret = unix_read_data(file, data ? data : (uint8_t *)"", length, flags, &rights, &metadata);
    if (already) metadata.credentials = (struct ucred){task->socket_receive_pid,
                                  task->socket_receive_uid, task->socket_receive_gid};
    if (ret < 0) {
        if (data) kernel_free(data);
        ret = (int)unix_receive_progress(task, file, total, ret, flags, &metadata);
        if (ret < 0) return ret;
        goto complete_message;
    }
    uint32_t copied = 0;
    uint32_t available = (uint32_t)ret < length ? (uint32_t)ret : length;
    uint64_t skip = already;
    for (uint64_t i = 0; i < message.msg_iovlen && copied < available; ++i) {
        uint64_t n = message.msg_iov[i].iov_len;
        if (skip >= n) { skip -= n; continue; }
        uint64_t offset = skip;
        n -= skip;
        skip = 0;
        if (n > available - copied) n = available - copied;
        if (n && !user_range_writable((uintptr_t)message.msg_iov[i].iov_base + offset, n)) {
            kernel_free(data);
            if (rights && !(flags & MSG_PEEK)) unix_rights_free(rights);
            task_socket_cancel_receive(task);
            return already ? (int)already : -LINUX_EFAULT;
        }
        for (uint64_t j = 0; j < n; ++j) ((uint8_t *)message.msg_iov[i].iov_base)[offset + j] = data[copied++];
    }
    if (data) kernel_free(data);
    ret = (int)unix_receive_progress(task, file, total, ret, flags, &metadata);
    if (ret == KERNEL_SYSCALL_BLOCKED) {
        task->socket_receive_message = (uintptr_t)user_msg;
        task->socket_receive_control_capacity = message.msg_controllen;
        task->socket_receive_name_capacity = message.msg_namelen;
        task->socket_receive_name = (uintptr_t)message.msg_name;
        if (!already && metadata.path_len) {
            task->socket_receive_path_length = metadata.path_len + 2;
            task->socket_receive_path[0] = AF_UNIX;
            task->socket_receive_path[1] = 0;
            __builtin_memcpy(task->socket_receive_path + 2, metadata.path, metadata.path_len);
        }
        task->socket_receive_flags = metadata.flags | (flags & MSG_CMSG_CLOEXEC);
        /* Header and control outputs are written only on completion, including
         * a signal-interrupted WAITALL returning its already-copied bytes. */
        return ret;
    }
complete_message: ;
    uint64_t capacity = message.msg_controllen;
    message.msg_controllen = 0;
    message.msg_flags = metadata.flags | (flags & MSG_CMSG_CLOEXEC);
    if (message.msg_name && metadata.path_len) {
        uint32_t name_length = metadata.path_len + 2;
        uint32_t n = message.msg_namelen < name_length ? message.msg_namelen : name_length;
        unsigned char bytes[111] = {AF_UNIX, 0};
        for (unsigned i = 0; i < metadata.path_len; ++i) bytes[i + 2] = metadata.path[i];
        for (unsigned i = 0; i < n; ++i) ((unsigned char *)message.msg_name)[i] = bytes[i];
        message.msg_namelen = name_length;
    } else message.msg_namelen = 0;
    if (passcred) message.msg_controllen = unix_put_credentials(&message, capacity, &metadata.credentials);
    uint8_t *control = (uint8_t *)message.msg_control + message.msg_controllen;
    capacity -= message.msg_controllen;
    if (rights) {
        uint32_t count = 0;
        uint64_t limit = capacity >= CMSG_LEN(0) ? (capacity - CMSG_LEN(0)) / sizeof(int) : 0;
        while (count < rights->count && count < limit) {
            int fd = rights->files[count] ? unix_clone_fd_to_task(task, rights->files[count]) :
                task_pty_import_fd(task, &rights->ptys[count],
                                   flags & MSG_CMSG_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0);
            if (fd < 0) break;
            if ((flags & MSG_CMSG_CLOEXEC) && rights->files[count])
                task_descriptor_for_fd(task, fd)->fd_flags = RELIEFOS_FD_CLOEXEC;
            uint8_t *slot = control + CMSG_LEN(0) + count * sizeof(int);
            for (unsigned i = 0; i < sizeof(int); ++i) slot[i] = ((uint8_t *)&fd)[i];
            ++count;
        }
        if (count) {
            struct cmsghdr header = {.cmsg_len = CMSG_LEN(count * sizeof(int)),
                .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS};
            for (unsigned i = 0; i < sizeof(header); ++i)
                control[i] = ((uint8_t *)&header)[i];
            uint64_t size = CMSG_SPACE(count * sizeof(int));
            message.msg_controllen += size < capacity ? size : capacity;
        }
        if (count < rights->count) message.msg_flags |= MSG_CTRUNC;
        if (!(flags & MSG_PEEK)) unix_rights_free(rights);
    }
    /* Match ____sys_recvmsg's output order without rewriting input pointers
     * and padding. Header output faults occur after the receive side effects. */
    struct msghdr *destination = (struct msghdr *)(uintptr_t)user_msg;
    if (message.msg_name) {
        if (!user_range_writable((uintptr_t)&destination->msg_namelen, sizeof(message.msg_namelen)))
            return -LINUX_EFAULT;
        __builtin_memcpy(&destination->msg_namelen, &message.msg_namelen, sizeof(message.msg_namelen));
    }
    if (!user_range_writable((uintptr_t)&destination->msg_flags, sizeof(message.msg_flags))) return -LINUX_EFAULT;
    __builtin_memcpy(&destination->msg_flags, &message.msg_flags, sizeof(message.msg_flags));
    if (!user_range_writable((uintptr_t)&destination->msg_controllen, sizeof(message.msg_controllen))) return -LINUX_EFAULT;
    __builtin_memcpy(&destination->msg_controllen, &message.msg_controllen, sizeof(message.msg_controllen));
    output->flags = (uint32_t)message.msg_flags;
    return ret;
}
#endif

/**
 * @brief Preserve common single-message behavior for native message batches.
 * @param task Current task under the execution lock.
 * @param file Pinned open socket description.
 * @param message User native msghdr address.
 * @param flags Flags supplied to this message operation.
 * @param receiving Select recvmsg instead of sendmsg.
 * @param batch Permit per-message MSG_EOR on sendmmsg.
 * @param result Captured input length and output flags used by batch iteration.
 * @return Bytes processed, negative errno, or the internal blocking sentinel.
 */
static int64_t inet_message(struct task_file *file, uint64_t user_header,
                            struct msghdr message, uint32_t flags, bool receiving,
                            uint64_t total, struct socket_message_result *output)
{
    bool packet = file->kind == TASK_FILE_KIND_PACKET;
    bool datagram = file->kind == TASK_FILE_KIND_UDP || packet;
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_CMSG_CLOEXEC))
        return -LINUX_EOPNOTSUPP;
    if (!receiving && message.msg_controllen) return -LINUX_EINVAL;
    if (!receiving && datagram && total > (packet ? 1514u : 1472u)) return -LINUX_EMSGSIZE;
#ifdef CONFIG_SWAP
    struct task *task=sched_current_task();
    uint32_t native_name_size=packet ? task_packet_address_size(file) : sizeof(struct sockaddr_in);
    if(packet && !native_name_size)return -LINUX_EBADF;
    if(!receiving && datagram && message.msg_name && message.msg_namelen>=native_name_size) {
        int prep=socket_user_prepare(task,(uintptr_t)message.msg_name,native_name_size,false);
        if(prep==KERNEL_SYSCALL_SWAP_CONTENDED)return prep;
    }
#endif
#ifndef CONFIG_SWAP
    if (receiving && !user_range_writable(user_header, sizeof(message))) return -LINUX_EFAULT;
#endif
    uint32_t size = total > 16384 ? 16384 : (uint32_t)total;
    uint8_t *data = size ? kernel_malloc(size) : NULL;
    if (size && !data) return -LINUX_ENOMEM;
    uint8_t *payload=data ? data : (uint8_t *)"";
    int64_t ret = -LINUX_EFAULT;
    uint32_t offset = 0;
    uint32_t output_size=size;
#ifdef CONFIG_SWAP
    if(receiving) {
        int probe=inet_receive_probe(file,size,datagram ? flags&~MSG_CMSG_CLOEXEC : flags,&output_size);
        if(probe<0){ret=probe;goto done;}
        int prep=socket_user_prepare(task,user_header,sizeof(message),true);
        if(prep){ret=prep;goto done;}
        if(!user_range_writable(user_header,sizeof(message)))goto done;
    }
#endif
    for (uint64_t i = 0; i < message.msg_iovlen && offset < (receiving ? output_size : size); ++i) {
        uint32_t limit=receiving ? output_size : size;
        uint32_t n = message.msg_iov[i].iov_len > limit - offset ? limit - offset : message.msg_iov[i].iov_len;
        uint64_t pointer = (uintptr_t)message.msg_iov[i].iov_base;
#ifdef CONFIG_SWAP
        if(n && receiving) {
            int prep=socket_user_prepare(task,pointer,n,true);
            if(prep){ret=prep;goto done;}
        }
        if(n && !receiving) {
            struct task_user_text part={0};
            int prep=task_lookup_user_bytes(task,pointer,n,&part);
            if(prep){ret=prep;goto done;}
            __builtin_memcpy(data+offset,part.text,n);kernel_free(part.allocation);
        }
#endif
#ifdef CONFIG_SWAP
        if (n && receiving && !user_range_writable(pointer, n)) goto done;
#else
        if (n && !(receiving ? user_range_writable(pointer, n) : user_range_ok(pointer, n))) goto done;
        if (!receiving && n) __builtin_memcpy(data + offset, (void *)(uintptr_t)pointer, n);
#endif
        offset += n;
    }
#ifdef CONFIG_SWAP
    if(receiving && datagram && message.msg_name) {
        uint32_t copied=message.msg_namelen<native_name_size ? message.msg_namelen : native_name_size;
        int prep=socket_user_prepare(task,(uintptr_t)message.msg_name,copied,true);
        if(prep){ret=prep;goto done;}
        if(copied && !user_range_writable((uintptr_t)message.msg_name,copied))goto done;
    }
#endif
    output->requested = total;
    struct task_file local = *file;
    if (flags & MSG_DONTWAIT) local.flags |= RELIEFOS_O_NONBLOCK;
    struct msghdr *destination = (void *)(uintptr_t)user_header;
    if (receiving) {
#ifdef CONFIG_SWAP
        union {struct sockaddr_in in;struct sockaddr_ll ll;} source={0};
        uint32_t source_length=0;
        if(packet)ret=task_packet_recv_staged(file,payload,output_size,(flags & ~MSG_CMSG_CLOEXEC)|MSG_TRUNC,
                                               message.msg_name ? &source : NULL,&source_length);
        else if(datagram) {
            ret=task_udp_recv_staged(file,payload,output_size,(flags & ~MSG_CMSG_CLOEXEC)|MSG_TRUNC,
                                     message.msg_name ? &source.in : NULL);
            source_length=sizeof(source.in);
        } else ret=task_inet_recv(file,payload,output_size,flags);
#else
        if (packet) ret = task_packet_recv(file, payload, size, (flags & ~MSG_CMSG_CLOEXEC) | MSG_TRUNC,
            (uintptr_t)message.msg_name, (uintptr_t)&destination->msg_namelen);
        else if (datagram) ret = task_udp_recv(file, payload, size, (flags & ~MSG_CMSG_CLOEXEC) | MSG_TRUNC,
            (uintptr_t)message.msg_name, (uintptr_t)&destination->msg_namelen);
        else ret = task_inet_recv(file, payload, size, flags);
#endif
        if (ret < 0) goto done;
#ifdef CONFIG_SWAP
        if(datagram && message.msg_name) {
            uint32_t copied_name=message.msg_namelen<source_length ? message.msg_namelen : source_length;
            if(copied_name)__builtin_memcpy(message.msg_name,&source,copied_name);
            destination->msg_namelen=source_length;
        }
#endif
        uint32_t copied = (uint64_t)ret < output_size ? (uint32_t)ret : output_size;
        offset = 0;
        for (uint64_t i = 0; i < message.msg_iovlen && offset < copied; ++i) {
            uint32_t n = message.msg_iov[i].iov_len > copied - offset ? copied - offset : message.msg_iov[i].iov_len;
            if (n) __builtin_memcpy(message.msg_iov[i].iov_base, data + offset, n);
            offset += n;
        }
        output->flags = datagram && (uint64_t)ret > size ? MSG_TRUNC : 0;
        destination->msg_flags = output->flags;
        destination->msg_controllen = 0;
        if (!message.msg_name || !datagram) destination->msg_namelen = 0;
        if (!(flags & MSG_TRUNC)) ret = copied;
    } else {
        if (packet) ret = task_packet_send(file, payload, size, flags, (uintptr_t)message.msg_name, message.msg_namelen);
        else if (datagram) ret = task_udp_send(file, payload, size, flags, (uintptr_t)message.msg_name, message.msg_namelen);
        else ret = task_inet_write(&local, payload, size);
    }
done:
    if (data) kernel_free(data);
    return ret;
}

/** @brief Transfer one native message through the original held socket.
 * @param task Borrowed caller under execution ownership and OFD admission.
 * @param file Original pinned socket description.
 * @param message User msghdr input/output address.
 * @param flags Native MSG flags; unsupported combinations return errno.
 * @param receiving True for recvmsg; false snapshots outgoing header/iovecs.
 * @param batch True when sendmmsg/recvmmsg owns progress across messages.
 * @param result Required requested-byte/result-flag output.
 * @return Bytes, negative errno or the internal blocking sentinel. Outgoing
 * header/vector/address/path operands remain task-owned across waits; their
 * copied pointer metadata is not re-imported from a changed user header.
 * Backend paths retain providers; no graph/module lock spans socket or I/O
 * callbacks. Temporary vector copies are freed on every outcome.
 */
int64_t task_socket_message(struct task *task, struct task_file *file, uint64_t message,
                            uint32_t flags, bool receiving, bool batch,
                            struct socket_message_result *result)
{
    if (flags & MSG_CMSG_COMPAT) return -LINUX_EINVAL;
    if (!file) return -LINUX_EBADF;
    if (!(file->flags & TASK_FILE_FLAG_SOCKET)) return -LINUX_ENOTSOCK;
    if (!(file->flags & (TASK_FILE_FLAG_SOCKET_UNIX | TASK_FILE_FLAG_SOCKET_INET))) return -LINUX_EOPNOTSUPP;
    struct msghdr imported;
    if(receiving) {
#ifdef CONFIG_SWAP
        struct task_user_text header={0};
        int rc=task_lookup_user_bytes(task,message,sizeof(imported),&header);if(rc)return inet_user_wait(task,file,rc);
        __builtin_memcpy(&imported,header.text,sizeof(imported));kernel_free(header.allocation);
#else
        if(!user_range_ok(message,sizeof(imported)))return -LINUX_EFAULT;
        __builtin_memcpy(&imported,(const void *)(uintptr_t)message,sizeof(imported));
#endif
    } else {
        struct task_user_text header={0};
        int rc=task_lookup_user_bytes(task,message,sizeof(imported),&header);
#ifdef CONFIG_SWAP
        if(rc)return inet_user_wait(task,file,rc);
#else
        if(rc)return rc;
#endif
        __builtin_memcpy(&imported,header.text,sizeof(imported));kernel_free(header.allocation);
    }
    if ((int32_t)imported.msg_namelen < 0) return -LINUX_EINVAL;
    if (imported.msg_namelen > 128) imported.msg_namelen = 128;
    if (!receiving && !imported.msg_namelen) imported.msg_name = NULL;
    if (imported.msg_iovlen > 1024) return -LINUX_EMSGSIZE;
    struct iovec fast[8], *vectors = fast;
    uint64_t size = imported.msg_iovlen * sizeof(*vectors);
    if (imported.msg_iovlen > 8) {
        vectors = kernel_malloc(size);
        if (!vectors) return -LINUX_ENOMEM;
    }
    int64_t status = -LINUX_EFAULT;
    if(size
#ifndef CONFIG_SWAP
       && !receiving
#endif
       ) {
        struct task_user_text copied={0};
        int rc=task_lookup_user_bytes(task,(uintptr_t)imported.msg_iov,size,&copied);
        if(rc){if(vectors!=fast)kernel_free(vectors);
#ifdef CONFIG_SWAP
            return inet_user_wait(task,file,rc);
#else
            return rc;
#endif
        }
        __builtin_memcpy(vectors,copied.text,size);kernel_free(copied.allocation);
    }
#ifndef CONFIG_SWAP
    if(receiving && size) {
        if(!user_range_ok((uintptr_t)imported.msg_iov,size)) {
            if(vectors!=fast)kernel_free(vectors);return -LINUX_EFAULT;
        }
        __builtin_memcpy(vectors,imported.msg_iov,size);
    }
#endif
    imported.msg_iov = vectors;
    uint64_t total;
    status = unix_message_vectors(&imported, &total);
    if (!status && (file->flags & TASK_FILE_FLAG_SOCKET_INET))
        status = inet_message(file, message, imported, flags, receiving, total, result);
    else if (!status) status = receiving ?
#ifdef CONFIG_SWAP
            unix_recvmsg_staged(task,file,message,0,imported,flags,total,result) :
#else
            unix_recvmsg(task, file, (const void *)(uintptr_t)message, imported, flags, total, result) :
#endif
        unix_sendmsg(task, file, imported, flags, batch, total, result);
    if (vectors != fast) kernel_free(vectors);
#ifdef CONFIG_SWAP
    return inet_user_wait(task,file,status);
#else
    return status;
#endif
}

/**
 * @brief Access the socket error retained by Linux recvmmsg after partial success.
 * @param file Pinned socket description.
 * @param error Positive errno to store when setting.
 * @param setting Store instead of consuming the error.
 * @return Previous error for reads, zero for writes or a missing socket.
 */
int task_socket_message_error(struct task_file *file, int error, bool setting)
{
    struct unix_socket *socket = unix_from_file(file);
    if (!socket) return 0;
    if (setting) { socket->error = error; return 0; }
    int previous = socket->error;
    socket->error = 0;
    return previous;
}

/**
 * @brief Dispatch one Linux AF_UNIX operation on the calling task's descriptor.
 * @param number Linux socket syscall number.
 * @param a0 First native syscall argument.
 * @param a1 Second native syscall argument.
 * @param a2 Third native syscall argument.
 * @param a3 Fourth native syscall argument, including accept4 flags.
 * @return Linux syscall result or KERNEL_SYSCALL_BLOCKED for scheduler retry.
 * @note The caller holds the kernel execution transaction. Blocking socket
 * operations enqueue the task before parking and preserve pinned-file state.
 */
static int64_t unix_socket_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3)
{
    struct task *task = sched_current_task();
    struct task_file *file;
    struct unix_socket *socket;
    uint32_t accept4_flags = (number == __NR_accept4) ? (uint32_t)a3 : 0u;
    (void)a3;
    if (!task) return -RELIEFOS_EPERM;
    uint32_t type = (uint32_t)a1 & 15;
    if (type == SOCK_RAW) type = SOCK_DGRAM;
    if (number == __NR_socket) {
        if (a1 & ~(uint32_t)(15 | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -RELIEFOS_EINVAL;
        if ((int)a0 != AF_UNIX) return -LINUX_EAFNOSUPPORT;
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) return -LINUX_ESOCKTNOSUPPORT;
        if (a2 != 0 && a2 != AF_UNIX) return -LINUX_EPROTONOSUPPORT;
        socket = unix_alloc(task->pid, type);
        if (!socket) return -RELIEFOS_ENOMEM;
        {
            int fd = unix_alloc_fd(task, socket);
            if (fd < 0) {
                unix_release_handle(socket->handle);
                return fd;
            }
            /* The allocation starts with an owner reference and adds one for the fd. */
            unix_release_handle(socket->handle);
            struct task_file *created = task_file_for_fd(task, fd);
            if (a1 & SOCK_NONBLOCK) created->flags |= RELIEFOS_O_NONBLOCK;
            if (a1 & SOCK_CLOEXEC) created->fd_flags = 1;
            return fd;
        }
    }
    if (number == __NR_socketpair) {
        struct unix_socket *left;
        struct unix_socket *right;
        int left_fd;
        int right_fd;
        int prep=socket_user_prepare(task,a3,sizeof(int)*2u,true);if(prep)return prep;
        if (!user_range_writable(a3, sizeof(int) * 2u)) return -RELIEFOS_EFAULT;
        if ((int)a0 != AF_UNIX || (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) ||
            (a2 != 0 && a2 != AF_UNIX) || (a1 & ~(uint32_t)(15 | SOCK_NONBLOCK | SOCK_CLOEXEC))) {
            return -RELIEFOS_EINVAL;
        }
        left = unix_alloc(task->pid, type);
        if (!left) return -RELIEFOS_ENOMEM;
        right = unix_alloc(task->pid, type);
        if (!right) {
            unix_release_handle(left->handle);
            return -RELIEFOS_ENOMEM;
        }
        left->state = UNIX_SOCKET_CONNECTED;
        right->state = UNIX_SOCKET_CONNECTED;
        left->peer_handle = right->handle;
        right->peer_handle = left->handle;
        left->peer_pid = task->pid;
        right->peer_pid = task->pid;
        left->peer_credentials = right->credentials;
        right->peer_credentials = left->credentials;
        left_fd = unix_alloc_fd(task, left);
        right_fd = unix_alloc_fd(task, right);
        unix_release_handle(left->handle);
        unix_release_handle(right->handle);
        if (left_fd < 0 || right_fd < 0) {
            if (left_fd >= 0) task_discard_file_fd(task, left_fd);
            if (right_fd >= 0) task_discard_file_fd(task, right_fd);
            return -RELIEFOS_EMFILE;
        }
        ((int *)(uintptr_t)a3)[0] = left_fd;
        ((int *)(uintptr_t)a3)[1] = right_fd;
        for (unsigned i = 0; i < 2; ++i) {
            struct task_file *created = task_file_for_fd(task, i ? right_fd : left_fd);
            if (a1 & SOCK_NONBLOCK) created->flags |= RELIEFOS_O_NONBLOCK;
            if (a1 & SOCK_CLOEXEC) created->fd_flags = 1;
        }
        return 0;
    }
    file = task_file_for_io(task, (int)a0);
    socket = unix_from_file(file);
    if (!socket) return -RELIEFOS_EBADF;
    if (number == __NR_bind) {
        struct unix_address target={0};
        if(socket->path_len)return -RELIEFOS_EINVAL;
        int ret=syscall_unix_address(vfs_system_namespace(),task,a1,(uint32_t)a2,true,&target);
        if(ret)return ret;
        if(!target.length){unix_autobind(socket);return 0;}
        if(!vfs_path_node(&target.object) && unix_find_path(target.path,target.length,NULL))
            return -RELIEFOS_EADDRINUSE;
        socket->bound_inode=target.object;
        for(unsigned i=0;i<target.length;++i)socket->path[i]=target.path[i];
        socket->path_len=target.length;socket->bound=true;return 0;
    }
    if (number == __NR_listen) {
        uint32_t requested_backlog = (uint32_t)(int32_t)a1;
        if (socket->type == SOCK_DGRAM) return -LINUX_EOPNOTSUPP;
        if (!socket->path_len || socket->state == UNIX_SOCKET_CONNECTED) return -RELIEFOS_EINVAL;
        if (requested_backlog > UNIX_SOCKET_BACKLOG_MAX)
            requested_backlog = UNIX_SOCKET_BACKLOG_MAX;
        socket->backlog = requested_backlog;
        /* Linux AF_UNIX accepts one more pending connection than backlog. */
        socket->pending_limit = requested_backlog + 1u;
        socket->state = UNIX_SOCKET_LISTEN;
        socket->credentials = (struct ucred){(int32_t)sched_task_tgid(task), task->euid, task->egid};
        kernel_poll_readiness_notify();
        return 0;
    }
    if (number == __NR_connect) {
        if (socket->passcred) unix_autobind(socket);
        if (socket->type == SOCK_DGRAM) {
            if (a2 < 2 || !user_range_ok(a1, 2)) return -RELIEFOS_EINVAL;
            if (*(const sa_family_t *)(uintptr_t)a1 == AF_UNSPEC) {
                socket->peer_handle = 0;
                socket->peer_path_len = 0;
                socket->state = UNIX_SOCKET_OPEN;
                kernel_poll_readiness_notify();
                return 0;
            }
            struct unix_socket *peer;
            int ret = unix_packet_peer(socket, (const void *)(uintptr_t)a1, (uint32_t)a2, &peer);
            if (ret < 0) return ret;
            socket->peer_handle = peer->handle;
            socket->peer_path_len = peer->path_len;
            for (unsigned i = 0; i < peer->path_len; ++i) socket->peer_path[i] = peer->path[i];
            socket->state = UNIX_SOCKET_CONNECTED;
            kernel_poll_readiness_notify();
            return 0;
        }
        struct unix_address target={0};
        struct unix_socket *listener,*server;
        if(socket->state!=UNIX_SOCKET_OPEN)return -RELIEFOS_EISCONN;
        int ret=syscall_unix_address(vfs_system_namespace(),task,a1,(uint32_t)a2,false,&target);
        if(ret)return ret;
        if(!target.length)return -RELIEFOS_EINVAL;
        listener=unix_find_path(target.path,target.length,&target.object);
        vfs_path_put(&target.object);
        if (listener && listener->type != socket->type) return -LINUX_EPROTOTYPE;
        if (!listener || listener->state != UNIX_SOCKET_LISTEN) return -LINUX_ECONNREFUSED;
        kernel_wait_queue_remove(&listener->wait_connect, task);
        if (listener->pending_count >= listener->pending_limit) {
            return unix_block(socket, file, &listener->wait_connect, false, 0);
        }
        ret = unix_pending_reserve(listener, listener->pending_count + 1u);
        if (ret < 0) return ret;
        server = unix_alloc(listener->owner_pid, listener->type);
        if (!server) return -RELIEFOS_ENOMEM;
        server->state = UNIX_SOCKET_CONNECTED;
        server->passcred = listener->passcred;
        server->unaccepted = 1;
        socket->state = UNIX_SOCKET_CONNECTED;
        socket->peer_handle = server->handle;
        socket->peer_pid = listener->owner_pid;
        server->peer_handle = socket->handle;
        server->peer_pid = task->pid;
        server->peer_credentials = (struct ucred){(int32_t)sched_task_tgid(task), task->euid, task->egid};
        socket->peer_credentials = listener->credentials;
        server->credentials = listener->credentials;
        server->path_len = listener->path_len;
        for (unsigned i = 0; i < listener->path_len; ++i) server->path[i] = listener->path[i];
        socket->peer_path_len = server->path_len;
        for (unsigned i = 0; i < server->path_len; ++i) socket->peer_path[i] = server->path[i];
        server->peer_path_len = socket->path_len;
        for (unsigned i = 0; i < socket->path_len; ++i) server->peer_path[i] = socket->path[i];
        listener->pending[listener->pending_count++] = server->handle;
        /* Listening sockets use the same receive generation as streams so an
         * ET epoll waiter observes a second connection even when accept()
         * and the next connect() happen between two readiness probes. */
        ++listener->rx_written;
        (void)kernel_wait_queue_wake_one(&listener->wait_accept);
        (void)kernel_wait_queue_wake_one(&listener->wait_connect);
        kernel_poll_readiness_notify();
        return 0;
    }
    if (number == __NR_accept4) {
        if (a3 & ~(uint32_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -RELIEFOS_EINVAL;
        number = __NR_accept;
    }
    if (number == __NR_accept) {
        kernel_wait_queue_remove(&socket->wait_accept, task);
        if(a1) {
            int prep=socket_user_prepare(task,a2,sizeof(socklen_t),true);
            if(prep)return prep;
        }
        if (a1 && !user_range_writable(a2, sizeof(socklen_t))) return -RELIEFOS_EFAULT;
        if (socket->state != UNIX_SOCKET_LISTEN) return -RELIEFOS_EINVAL;
        if (!socket->pending_count) {
            return unix_block(socket, file, &socket->wait_accept, true, 0);
        }
        uint32_t handle = socket->pending[0];
        struct unix_socket *accepted = unix_from_handle(handle);
        if (a1) {
            int ret = unix_address_out(unix_from_handle(accepted->peer_handle), a1, a2);
            if (ret < 0) return ret;
        }
        int fd = unix_alloc_fd(task, accepted);
        if (fd < 0) return fd;
        for (uint32_t i = 1; i < socket->pending_count; ++i) socket->pending[i - 1] = socket->pending[i];
        --socket->pending_count;
        {
            struct task_file *accepted_file = task_file_for_fd(task, fd);
            if (accepted_file) {
                if (accept4_flags & SOCK_NONBLOCK) accepted_file->flags |= RELIEFOS_O_NONBLOCK;
                if (accept4_flags & SOCK_CLOEXEC) accepted_file->fd_flags |= 1u;
            }
        }
        accepted->passcred = socket->passcred;
        accepted->unaccepted = 0;
        unix_release_handle(handle); /* drop the listener's pending reference */
        (void)kernel_wait_queue_wake_one(&socket->wait_connect);
        kernel_poll_readiness_notify();
        return fd;
    }
    if (number == __NR_getsockname || number == __NR_getpeername) {
        if (number == __NR_getpeername) {
            if (socket->state != UNIX_SOCKET_CONNECTED) return -LINUX_ENOTCONN;
            return unix_name_out(socket->peer_path, socket->peer_path_len, a1, a2);
        }
        return unix_address_out(socket, a1, a2);
    }
    if (number == __NR_setsockopt || number == __NR_getsockopt) return -LINUX_ENOPROTOOPT;
    if (number == __NR_shutdown) {
        if (a1 != SHUT_RD && a1 != SHUT_WR && a1 != SHUT_RDWR) return -RELIEFOS_EINVAL;
        if (a1 == SHUT_RD || a1 == SHUT_RDWR) socket->shutdown_read = 1;
        if (a1 == SHUT_WR || a1 == SHUT_RDWR) socket->shutdown_write = 1;
        struct unix_socket *peer = unix_from_handle(socket->peer_handle);
        if (peer && socket->type != SOCK_DGRAM) {
            if (a1 == SHUT_RD || a1 == SHUT_RDWR) peer->shutdown_write = 1;
            if (a1 == SHUT_WR || a1 == SHUT_RDWR) peer->shutdown_read = 1;
        }
        (void)kernel_wait_queue_wake_all(&socket->wait_rx);
        (void)kernel_wait_queue_wake_all(&socket->wait_tx);
        unix_wake_peer(socket);
        return 0;
    }
    return -RELIEFOS_ENOSYS;
}

/**
 * @brief Read or configure supported AF_UNIX socket-level options.
 * @param socket Stable AF_UNIX socket object.
 * @param setting Select setsockopt when true, getsockopt otherwise.
 * @param level Socket option level.
 * @param option Option identifier.
 * @param address User option value address.
 * @param length Option length or getsockopt socklen_t pointer.
 * @return Zero on success or a negative Linux errno.
 * @note SO_*TIMEO uses scheduler ticks, rounds positive values up, maps an
 * all-zero timeval to an infinite wait, and maps negative seconds to an
 * immediate timeout that getsockopt reports as (0, 0).
 */
static int unix_socket_option(struct unix_socket *socket, bool setting, int level,
                               int option, uint64_t address, uint64_t length)
{
    if (setting) {
        if ((int32_t)length < 0) return -RELIEFOS_EINVAL;
        if (level != SOL_SOCKET) return -LINUX_ENOPROTOOPT;
        if (option == SO_RCVTIMEO || option == SO_RCVTIMEO_NEW ||
            option == SO_SNDTIMEO || option == SO_SNDTIMEO_NEW) {
            if (length < sizeof(struct linux_timeval)) return -RELIEFOS_EINVAL;
            int prep=socket_user_prepare(sched_current_task(),address,sizeof(struct linux_timeval),false);
            if(prep)return prep;
            if (!user_range_ok(address, sizeof(struct linux_timeval))) return -RELIEFOS_EFAULT;
            struct linux_timeval timeout = *(const struct linux_timeval *)(uintptr_t)address;
            if (timeout.tv_usec < 0 || timeout.tv_usec >= 1000000)
                return -LINUX_EDOM;
            uint64_t ticks;
            if (timeout.tv_sec < 0) {
                ticks = 0;
            } else if (!timeout.tv_sec && !timeout.tv_usec) {
                ticks = UINT64_MAX;
            } else {
                uint64_t seconds = (uint64_t)timeout.tv_sec;
                uint64_t fraction = ((uint64_t)timeout.tv_usec * RELIEFNT_TICK_HZ + 999999u) /
                                    1000000u;
                if (seconds > (UINT64_MAX - 1u - fraction) / RELIEFNT_TICK_HZ)
                    ticks = UINT64_MAX - 1u;
                else
                    ticks = seconds * RELIEFNT_TICK_HZ + fraction;
            }
            if (option == SO_RCVTIMEO || option == SO_RCVTIMEO_NEW) socket->receive_timeout = ticks;
            else socket->send_timeout = ticks;
            return 0;
        }
        if (option != SO_PASSCRED && option != SO_RCVLOWAT) return -LINUX_ENOPROTOOPT;
        if (length < sizeof(int32_t)) return -RELIEFOS_EINVAL;
        int prep=socket_user_prepare(sched_current_task(),address,sizeof(int32_t),false);
        if(prep)return prep;
        if (!user_range_ok(address, sizeof(int32_t))) return -RELIEFOS_EFAULT;
        int32_t value = *(const int32_t *)(uintptr_t)address;
        if (option == SO_PASSCRED) socket->passcred = value != 0;
        else socket->receive_lowwater = value < 0 ? INT32_MAX : value ? (uint32_t)value : 1;
        return 0;
    }
    socklen_t capacity;int prep=socket_user_length(sched_current_task(),length,&capacity);
    if(prep)return prep;
    if ((int32_t)capacity < 0) return -RELIEFOS_EINVAL;
    if (level != SOL_SOCKET) return -LINUX_ENOPROTOOPT;
    int32_t value = 0;
    struct linux_timeval timeout = {0};
    const void *data = &value;
    uint32_t size = sizeof(value);
    if(option==SO_ERROR) {
        uint32_t copied=capacity<sizeof(value) ? capacity : sizeof(value);
        prep=socket_user_prepare(sched_current_task(),address,copied,true);
        if(prep==KERNEL_SYSCALL_SWAP_CONTENDED)return prep;
    }
    switch (option) {
    case SO_PEERCRED: data = &socket->peer_credentials; size = sizeof(socket->peer_credentials); break;
    case SO_TYPE: value = (int32_t)socket->type; break;
    case SO_ERROR: value = socket->error; socket->error = 0; break;
    case SO_PASSCRED: value = (int32_t)socket->passcred; break;
    case SO_RCVLOWAT: value = (int32_t)socket->receive_lowwater; break;
    case SO_SNDLOWAT: value = 1; break;
    case SO_ACCEPTCONN: value = socket->state == UNIX_SOCKET_LISTEN; break;
    case SO_DOMAIN: value = AF_UNIX; break;
    case SO_PROTOCOL: value = 0; break;
    case SO_RCVTIMEO:
    case SO_RCVTIMEO_NEW:
    case SO_SNDTIMEO:
    case SO_SNDTIMEO_NEW: {
        uint64_t ticks = option == SO_RCVTIMEO || option == SO_RCVTIMEO_NEW
            ? socket->receive_timeout : socket->send_timeout;
        if (ticks != UINT64_MAX) {
            timeout.tv_sec = (int64_t)(ticks / RELIEFNT_TICK_HZ);
            timeout.tv_usec = (int64_t)((ticks % RELIEFNT_TICK_HZ) * (1000000 / RELIEFNT_TICK_HZ));
        }
        data = &timeout;
        size = sizeof(timeout);
        break;
    }
    default:
        return -LINUX_ENOPROTOOPT;
    }
    if (size > capacity) size = capacity;
    prep=socket_user_prepare(sched_current_task(),address,size,true);if(prep)return prep;
    if (size && !user_range_writable(address, size)) return -RELIEFOS_EFAULT;
    for (unsigned i = 0; i < size; ++i) ((uint8_t *)(uintptr_t)address)[i] = ((const uint8_t *)data)[i];
    *(socklen_t *)(uintptr_t)length = size;
    return 0;
}

/**
 * @brief Route native socket syscalls to AF_UNIX, TCP, UDP, and packet handlers.
 * @param number Linux syscall number.
 * @param a0 First native syscall argument.
 * @param a1 Second native syscall argument.
 * @param a2 Third native syscall argument.
 * @param a3 Fourth native syscall argument.
 * @param a4 Fifth native syscall argument.
 * @param a5 Sixth native syscall argument.
 * @return Linux syscall result or the internal blocking sentinel.
 * @note The syscall entry owns the global execution transaction and keeps a
 * task-file pin across retries; completion clears per-syscall timeout state.
 */
int64_t syscall_socket_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    struct task *task;
    struct task_file *file;
    if (number == __NR_sendmmsg || number == __NR_recvmmsg)
        return syscall_socket_mmsg(number == __NR_recvmmsg, (int32_t)a0, a1,
                                    (uint32_t)a2, (uint32_t)a3, a4);
    /* These are int/unsigned int arguments in the native Linux syscall
     * prototypes; pointers and size_t byte counts retain all 64 bits. */
    if (number == __NR_socket || number == __NR_socketpair) {
        a0 = (uint32_t)a0;
        a1 = (uint32_t)a1;
        a2 = (uint32_t)a2;
    }
    if (number == __NR_accept4 || number == __NR_sendto || number == __NR_recvfrom)
        a3 = (uint32_t)a3;
    if (number == __NR_getsockopt || number == __NR_setsockopt) {
        task = sched_current_task();
        file = task_file_for_io(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        struct unix_socket *socket = unix_from_file(file);
        if (socket) return unix_socket_option(socket, number == __NR_setsockopt,
                                             (int)a1, (int)a2, a3, a4);
        if (file->flags & TASK_FILE_FLAG_SOCKET_INET)
            return inet_socket_dispatch(number, a0, a1, a2, a3, a4);
        return -LINUX_ENOTSOCK;
    }
    if (number == __NR_sendmsg || number == __NR_recvmsg) {
        task = sched_current_task();
        if ((uint32_t)a2 & MSG_CMSG_COMPAT) return -LINUX_EINVAL;
        file = number == __NR_recvmsg && task->socket_receive_file ? task->socket_receive_file :
            task_file_for_io(task, (int32_t)a0);
        struct socket_message_result output = {0};
        return task_socket_message(task, file, a1, (uint32_t)a2, number == __NR_recvmsg, false, &output);
    }
    if (number == __NR_sendto) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
#ifdef CONFIG_SWAP
        if (file->flags & TASK_FILE_FLAG_SOCKET_UNIX) {
            if (!inet_user_span(a1,a2)) return -RELIEFOS_EFAULT;
            if (a3 & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE | MSG_EOR)) return -LINUX_EOPNOTSUPP;
            struct iovec vector={.iov_base=(void *)(uintptr_t)a1,.iov_len=a2};
            struct msghdr message={.msg_name=(void *)(uintptr_t)a4,.msg_namelen=(uint32_t)a5,
                                   .msg_iov=&vector,.msg_iovlen=1};
            struct socket_message_result result={0};
            return unix_sendmsg(task,file,message,(uint32_t)a3,false,a2,&result);
        }
        if(file->flags & TASK_FILE_FLAG_SOCKET_INET) {
            if(!inet_user_span(a1,a2))return -RELIEFOS_EFAULT;
            if(a3 & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE | MSG_EOR))return -LINUX_EOPNOTSUPP;
            bool packet=file->kind==TASK_FILE_KIND_PACKET,udp=file->kind==TASK_FILE_KIND_UDP;
            if((packet && a2>1514u) || (udp && a2>1472u))return -LINUX_EMSGSIZE;
            uint32_t size=a2>32768u ? 32768u : (uint32_t)a2;
            struct task_user_text data={0};
            int rc=size ? task_lookup_user_bytes(task,a1,size,&data) : 0;
            if(rc)return inet_user_wait(task,file,rc);
            const void *bytes=size ? data.text : "";
            int ret;
            if(packet)ret=task_packet_send(file,bytes,size,(uint32_t)a3,a4,(uint32_t)a5);
            else if(udp)ret=task_udp_send(file,bytes,size,(uint32_t)a3,a4,(uint32_t)a5);
            else {
                struct task_file local=*file;
                if(a3 & MSG_DONTWAIT)local.flags|=RELIEFOS_O_NONBLOCK;
                ret=task_inet_write(&local,bytes,size);
                if(ret==-RELIEFOS_EPIPE && !(a3 & MSG_NOSIGNAL))sched_signal_user_task(task->pid,13);
            }
            kernel_free(data.allocation);
            return inet_user_wait(task,file,ret);
        }
#endif
        if (a2 && !user_range_ok(a1, a2)) return -RELIEFOS_EFAULT;
        if (file->flags & TASK_FILE_FLAG_SOCKET_INET) {
            if (a3 & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE | MSG_EOR))
                return -LINUX_EOPNOTSUPP;
            if (file->kind == TASK_FILE_KIND_PACKET)
                return task_packet_send(file, (void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3, a4, (uint32_t)a5);
            if (file->kind == TASK_FILE_KIND_UDP)
                return task_udp_send(file, (void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3, a4, (uint32_t)a5);
            struct task_file local = *file;
            if (a3 & MSG_DONTWAIT) local.flags |= RELIEFOS_O_NONBLOCK;
            int ret = task_inet_write(&local, (const void *)(uintptr_t)a1, (uint32_t)a2);
            if (ret == -RELIEFOS_EPIPE && !(a3 & MSG_NOSIGNAL))
                sched_signal_user_task(task->pid, 13);
            return ret;
        }
        if (!(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) return -RELIEFOS_EBADF;
        if (a3 & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_MORE | MSG_EOR)) return -LINUX_EOPNOTSUPP;
        struct unix_socket *socket = unix_from_file(file);
        if (socket->type != SOCK_STREAM) {
            struct unix_socket *peer;
            int result = unix_packet_peer(socket, (const void *)(uintptr_t)a4, (uint32_t)a5, &peer);
            if (result < 0) return result;
            if (a2 > 65536) return -LINUX_EMSGSIZE;
            int ret = unix_send_packet(file, peer, (const void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3, NULL);
            if (ret == -RELIEFOS_EPIPE && !(a3 & MSG_NOSIGNAL)) sched_signal_user_task(task->pid, 13);
            return ret;
        }
        if (a4) return -RELIEFOS_EISCONN;
        struct task_file local = *file;
        if (a3 & MSG_DONTWAIT) local.flags |= RELIEFOS_O_NONBLOCK;
        int ret = task_socket_write(&local, (const void *)(uintptr_t)a1, (uint32_t)a2);
        if (ret == -RELIEFOS_EPIPE && !(a3 & MSG_NOSIGNAL)) sched_signal_user_task(task->pid, 13);
        return ret;
    }
    if (number == __NR_recvfrom) {
        struct task *task = sched_current_task();
        struct task_file *file = task->socket_receive_file ? task->socket_receive_file : task_file_for_io(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
#ifdef CONFIG_SWAP
        if(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)
            return task_socket_receive_user(task,file,a1,a2,(uint32_t)a3,a4,a5);
        if(file->flags & TASK_FILE_FLAG_SOCKET_INET) {
            if(!inet_user_span(a1,a2))return -RELIEFOS_EFAULT;
            uint32_t cap=file->kind==TASK_FILE_KIND_PACKET ? 1514u :
                         file->kind==TASK_FILE_KIND_UDP ? 1472u : 32768u;
            uint32_t size=a2>cap ? cap : (uint32_t)a2;
            uint32_t actual=0;
            int observed=inet_receive_probe(file,size,(uint32_t)a3,&actual);
            if(observed<0)return inet_user_wait(task,file,observed);
            int rc=socket_user_prepare(task,a1,actual,true);
            if(rc)return inet_user_wait(task,file,rc);
            if(actual && !user_range_writable(a1,actual))return -RELIEFOS_EFAULT;
            int ret;
            if(file->kind==TASK_FILE_KIND_PACKET)
                ret=task_packet_recv(file,(void *)(uintptr_t)a1,actual,(uint32_t)a3,a4,a5);
            else if(file->kind==TASK_FILE_KIND_UDP)
                ret=task_udp_recv(file,(void *)(uintptr_t)a1,actual,(uint32_t)a3,a4,a5);
            else ret=task_inet_recv(file,(void *)(uintptr_t)a1,actual,(uint32_t)a3);
            return inet_user_wait(task,file,ret);
        }
#endif
        if (a2 && !user_range_writable(a1, a2)) return -RELIEFOS_EFAULT;
        if (file->flags & TASK_FILE_FLAG_SOCKET_INET) {
            if (file->kind == TASK_FILE_KIND_PACKET)
                return task_packet_recv(file, (void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3, a4, a5);
            if (file->kind == TASK_FILE_KIND_UDP)
                return task_udp_recv(file, (void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3, a4, a5);
            return task_inet_recv(file, (void *)(uintptr_t)a1, (uint32_t)a2, (uint32_t)a3);
        }
        if (!(file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) return -RELIEFOS_EBADF;
        if (a3 & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL | MSG_CMSG_CLOEXEC)) return -LINUX_EOPNOTSUPP;
        if (a4) {
            if (!user_range_writable(a5, 4)) return -RELIEFOS_EFAULT;
            uint32_t capacity = *(uint32_t *)(uintptr_t)a5;
            if ((int32_t)capacity < 0) return -RELIEFOS_EINVAL;
            if (!user_range_writable(a4, capacity < 111 ? capacity : 111)) return -RELIEFOS_EFAULT;
        }
        struct unix_receive_result metadata;
        uint64_t already = task->socket_receive_done;
        if (already > a2) { task_socket_cancel_receive(task); return -RELIEFOS_EINVAL; }
        uint64_t remaining = a2 - already;
        int ret = unix_read_data(file, (void *)(uintptr_t)(a1 + already),
                                remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining,
                                (uint32_t)a3, NULL, &metadata);
        ret = (int)unix_receive_progress(task, file, a2, ret, (uint32_t)a3, &metadata);
        if (ret >= 0 && a4) {
            if (metadata.path_len) (void)unix_name_out(metadata.path, metadata.path_len, a4, a5);
            else *(socklen_t *)(uintptr_t)a5 = 0;
        }
        return ret;
    }
    task = sched_current_task();
    if (number == __NR_socket && ((int)a0 == AF_INET || (int)a0 == AF_PACKET)) return inet_socket_dispatch(number, a0, a1, a2, a3, a4);
    file = task_file_for_io(task, (int)a0);
    if (number != __NR_socket && file && (file->flags & TASK_FILE_FLAG_SOCKET_INET)) {
        return inet_socket_dispatch(number, a0, a1, a2, a3, a4);
    }
    return unix_socket_dispatch(number, a0, a1, a2, a3);
}

/** @brief Render live UNIX socket metadata with no kernel pointer disclosure.
 * @param s Required slice; caller holds execution ownership over registry.
 * References, type, state and names come from actual live socket objects.
 */
void unix_proc_emit(struct text_stream *s)
{
    text_string(s, "Num       RefCount Protocol Flags    Type St Inode Path\n");
    for (struct unix_socket *u = unix_sockets; u; u = u->registry_next) {
        if (!u->used)
            continue;
        text_string(s, "0000000000000000: ");
        text_hex(s, u->refs, 8);
        text_string(s, " 00000000 ");
        text_hex(s, u->state == UNIX_SOCKET_LISTEN ? 0x10000 : 0, 8);
        text_string(s, " ");
        text_hex(s, u->type, 4);
        text_string(s, u->state == UNIX_SOCKET_CONNECTED ? " 03 " : " 01 ");
        text_unsigned(s, (1ULL << 60) | u->handle);
        if (u->path_len) {
            text_string(s, " ");
            for (uint32_t i = 0; i < u->path_len; ++i) {
                char c = u->path[i];
                if (!c) {
                    if (i == 0)
                        text_string(s, "@");
                    else if (i + 1 < u->path_len)
                        text_string(s, "@");
                } else if (c == '\n')
                    text_string(s, "\\n");
                else
                    text_bytes(s, &c, 1);
            }
        }
        text_string(s, "\n");
    }
}

#include "internal.h"


struct pty_session_page {
    struct pty_session_page *next;
    struct pty_session entries[PTY_SESSION_PAGE_SIZE];
};

/* Session objects never move or disappear while the kernel runs. New pages
 * extend the stable ID space without invalidating borrowers from find_session. */
static struct pty_session_page initial_session_page;
static struct pty_session_page *session_page_tail = &initial_session_page;
static uint32_t session_page_count = 1;
static struct kernel_spinlock session_page_lock;

static uint32_t next_generation;
uint8_t vt_ready;

uint32_t active_vt;

uint8_t vt_graphical[VT_COUNT];

struct vt_mode vt_modes[VT_COUNT];

int vt_keyboard_modes[VT_COUNT];

uint8_t vt_release_pending[VT_COUNT];

uint8_t vt_acquire_pending[VT_COUNT];

uint32_t vt_controller_pid[VT_COUNT];

short vt_relsig[VT_COUNT];

short vt_acqsig[VT_COUNT];

uint32_t vt_switch_target;

uint32_t console_modifier_keys;

uint8_t console_shift_down;

uint8_t console_ctrl_down;

uint8_t console_alt_down;

struct kernel_wait_queue vt_waiters;

uint64_t vt_display_generation;

/**
 * @brief Resolve a stable PTY slot through the linked session-page directory.
 * @param index Zero-based PTY ID index; zero is PTY ID 1.
 * @return Stable slot pointer when its page exists, otherwise NULL.
 */
struct pty_session *pty_session_slot_at(uint32_t index)
{
    uint32_t page_index = index / PTY_SESSION_PAGE_SIZE;
    if (page_index >= __atomic_load_n(&session_page_count, __ATOMIC_ACQUIRE))
        return NULL;
    struct pty_session_page *page = &initial_session_page;
    while (page_index--) {
        page = __atomic_load_n(&page->next, __ATOMIC_ACQUIRE);
        if (!page) return NULL;
    }
    return &page->entries[index % PTY_SESSION_PAGE_SIZE];
}

/**
 * @brief Return the number of numeric PTY IDs backed by stable session pages.
 * @return Addressable PTY slot count.
 */
uint32_t pty_session_slot_capacity(void)
{
    return __atomic_load_n(&session_page_count, __ATOMIC_ACQUIRE) *
           PTY_SESSION_PAGE_SIZE;
}

/**
 * @brief Return a live PTY session by its stable 1-based identifier.
 * @param pty_id 1-based PTY ID; zero is invalid.
 * @return Stable session pointer when allocated, otherwise NULL.
 */
struct pty_session *find_session(uint32_t pty_id)
{
    if (!pty_id) return NULL;
    struct pty_session *session = pty_session_slot_at(pty_id - 1u);
    return session && __atomic_load_n(&session->used, __ATOMIC_ACQUIRE)
        ? session : NULL;
}

int pty_get_node(uint32_t pty_id, struct storage_node *node)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return -2;
    if (node) *node = (struct storage_node){
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_PTY | STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = pty_id,
        .volume_id = session->generation,
    };
    return 0;
}

int pty_lookup_path(const char *path, struct storage_node *node)
{
    const char *prefix = "/dev/pts/";
    while (*prefix && *path == *prefix) { ++path; ++prefix; }
    if (*prefix || *path < '1' || *path > '9') return -2;
    uint32_t id = 0;
    do {
        uint32_t digit = (uint32_t)(*path++ - '0');
        if (id > (UINT32_MAX - digit) / 10u) return -2;
        id = id * 10u + digit;
        if (id > pty_session_slot_capacity()) return -2;
    } while (*path >= '0' && *path <= '9');
    struct pty_session *session = find_session(id);
    if (*path || !session || session->hungup || session->console) return -2;
    return pty_get_node(id, node);
}

/** @brief Resolve one of the six fixed Linux virtual consoles.
 * @param path Device pathname, nullable.
 * @param node Optional output storage node.
 * @return Zero if resolved or negative ENOENT. */
int pty_lookup_vt_path(const char *path, struct storage_node *node)
{
    static const char prefix[] = "/dev/tty";
    uint32_t i;
    if (!path || !vt_ready) return -2;
    for (i = 0; i < sizeof(prefix) - 1u; ++i) {
        if (path[i] != prefix[i]) return -2;
    }
    if (path[i] == '0') {
        if (path[i + 1u]) return -2;
        return pty_get_node(active_vt, node);
    }
    if (path[i] < '1' || path[i] > '6' || path[i + 1u]) return -2;
    return pty_get_node((uint32_t)(path[i] - '0'), node);
}

int pty_inode_permissions(const struct storage_node *node,
                           struct reliefos_permissions *value, bool write)
{
    struct pty_session *session = find_session(node->first_cluster);
    if (!session || session->generation != node->volume_id) return -2;
    if (write) session->permissions = *value;
    else *value = session->permissions;
    return 0;
}

/**
 * @brief Reset allocated PTY sessions while preserving stable backing pages.
 */
void pty_init(void)
{
    kernel_spin_init(&session_page_lock);
    kernel_wait_queue_init(&vt_waiters);
    vt_display_generation = 0;
    vt_ready = 0;
    active_vt = 0;
    for (uint32_t i = 0; i < VT_COUNT; ++i) {
        vt_graphical[i] = 0;
        vt_modes[i] = (struct vt_mode){ .mode = VT_AUTO };
        vt_keyboard_modes[i] = K_XLATE;
        vt_release_pending[i] = 0;
        vt_acquire_pending[i] = 0;
        vt_controller_pid[i] = 0;
        vt_relsig[i] = 0;
        vt_acqsig[i] = 0;
    }
    vt_switch_target = 0;
    console_modifier_keys = 0;
    console_shift_down = 0;
    console_ctrl_down = 0;
    console_alt_down = 0;
    for (uint32_t i = 0; i < pty_session_slot_capacity(); ++i) {
        struct pty_session *session = pty_session_slot_at(i);
        if (session) __builtin_memset(session, 0, sizeof(*session));
    }
}

/**
 * @brief Initialize a reserved session slot with default Unix98 terminal state.
 * @param session Stable, unused slot protected by session_page_lock.
 * @param owner_pid Task that opened ptmx; zero for fixed virtual consoles.
 * @param permissions Mode and filesystem credentials captured before taking the page lock.
 * @param winsize Initial terminal dimensions captured before taking the page lock.
 */
static void pty_initialize_session(struct pty_session *session, uint32_t owner_pid,
                                   const struct reliefos_permissions *permissions,
                                   const struct linux_winsize *winsize)
{
    __builtin_memset(session, 0, sizeof(*session));
    session->locked = 1;
    session->owner_pid = owner_pid;
    session->permissions = *permissions;
    if (!++next_generation) ++next_generation;
    session->generation = next_generation;
    session->termios.c_iflag = RELIEFOS_PTY_IFLAG_ICRNL;
    session->termios.c_oflag = LINUX_OPOST | LINUX_ONLCR;
    session->termios.c_cflag = LINUX_CLOCAL | LINUX_CREAD | LINUX_CS8 | LINUX_B115200;
    session->termios.c_lflag = RELIEFOS_PTY_LFLAG_ECHO |
                               RELIEFOS_PTY_LFLAG_ECHONL |
                               RELIEFOS_PTY_LFLAG_ICANON |
                               RELIEFOS_PTY_LFLAG_IEXTEN |
                               RELIEFOS_PTY_LFLAG_ISIG;
    session->termios.c_cc[RELIEFOS_PTY_CC_VEOF] = 4;
    session->termios.c_cc[RELIEFOS_PTY_CC_VEOL] = 0;
    session->termios.c_cc[RELIEFOS_PTY_CC_VERASE] = 127;
    session->termios.c_cc[RELIEFOS_PTY_CC_VINTR] = 3;
    session->termios.c_cc[RELIEFOS_PTY_CC_VKILL] = 21;
    session->termios.c_cc[RELIEFOS_PTY_CC_VMIN] = 1;
    session->termios.c_cc[RELIEFOS_PTY_CC_VQUIT] = 28;
    session->termios.c_cc[RELIEFOS_PTY_CC_VSTART] = 17;
    session->termios.c_cc[RELIEFOS_PTY_CC_VSTOP] = 19;
    session->termios.c_cc[RELIEFOS_PTY_CC_VSUSP] = 26;
    session->termios.c_cc[RELIEFOS_PTY_CC_VTIME] = 0;
    session->termios.c_ispeed = 115200U;
    session->termios.c_ospeed = 115200U;
    session->winsize = *winsize;
    /* Publish the fully initialized session to lock-free readers. */
    __atomic_store_n(&session->used, 1, __ATOMIC_RELEASE);
}

/**
 * @brief Allocate a PTY at a stable numeric ID, growing page storage as needed.
 * @param owner_pid Task that opened ptmx; zero is reserved for kernel consoles.
 * @return Stable 1-based PTY ID or negative ENOMEM when ID/storage growth fails.
 */
int32_t pty_create(uint32_t owner_pid)
{
    uint64_t flags;
    const struct task *owner = sched_find(owner_pid);
    struct reliefos_permissions permissions = {0600,
        owner ? owner->fsuid : 0, owner ? owner->fsgid : 0};
    const struct framebuffer *fb = framebuffer_get();
    uint32_t cols = fb && fb->available ? fb->width / RELIEFOS_FONT_W : 80u;
    uint32_t rows = fb && fb->available ? fb->height / RELIEFOS_FONT_H : 24u;
    struct linux_winsize winsize = {
        .ws_row = (uint16_t)(rows > 0xffffu ? 0xffffu : rows),
        .ws_col = (uint16_t)(cols > 0xffffu ? 0xffffu : cols),
    };
    if (!winsize.ws_row) winsize.ws_row = 24;
    if (!winsize.ws_col) winsize.ws_col = 80;
    for (uint32_t i = 0; i < pty_session_slot_capacity(); ++i) {
        struct pty_session *session = pty_session_slot_at(i);
        if (session && __atomic_load_n(&session->used, __ATOMIC_ACQUIRE) &&
            session->hungup && !session->console &&
            sched_pty_reference_count(i + 1u) == 0)
            pty_reap_hungup(i + 1u);
    }
    for (;;) {
        kernel_spin_lock_irqsave(&session_page_lock, &flags);
        uint32_t capacity = pty_session_slot_capacity();
        for (uint32_t i = 0; i < capacity; ++i) {
            struct pty_session *session = pty_session_slot_at(i);
            if (!session || __atomic_load_n(&session->used, __ATOMIC_ACQUIRE)) continue;
            pty_initialize_session(session, owner_pid, &permissions, &winsize);
            uint32_t process_session = session->process_session;
            uint32_t foreground_pgid = session->foreground_pgid;
            kernel_spin_unlock_irqrestore(&session_page_lock, flags);
            console_debug("[reliefnt] pty create owner=%u id=%u session=%u pgrp=%u\n",
                          owner_pid, i + 1u, process_session, foreground_pgid);
            return (int32_t)(i + 1u);
        }
        uint32_t page_count = __atomic_load_n(&session_page_count, __ATOMIC_RELAXED);
        /* pty_create returns a signed ID, so never publish IDs above INT32_MAX. */
        if (page_count >= (uint32_t)INT32_MAX / PTY_SESSION_PAGE_SIZE) {
            kernel_spin_unlock_irqrestore(&session_page_lock, flags);
            return -12;
        }
        kernel_spin_unlock_irqrestore(&session_page_lock, flags);

        /* Allocation is outside the spinlock. Another creator may append a
         * page first; in that case this zeroed candidate is simply discarded. */
        struct pty_session_page *page = kernel_malloc(sizeof(*page));
        if (!page) return -12;
        __builtin_memset(page, 0, sizeof(*page));
        kernel_spin_lock_irqsave(&session_page_lock, &flags);
        capacity = pty_session_slot_capacity();
        bool have_free = false;
        for (uint32_t i = 0; i < capacity; ++i) {
            struct pty_session *session = pty_session_slot_at(i);
            if (session && !__atomic_load_n(&session->used, __ATOMIC_ACQUIRE)) {
                have_free = true;
                break;
            }
        }
        page_count = __atomic_load_n(&session_page_count, __ATOMIC_RELAXED);
        if (have_free) {
            kernel_spin_unlock_irqrestore(&session_page_lock, flags);
            kernel_free(page);
            continue;
        }
        if (page_count >= (uint32_t)INT32_MAX / PTY_SESSION_PAGE_SIZE) {
            kernel_spin_unlock_irqrestore(&session_page_lock, flags);
            kernel_free(page);
            return -12;
        }
        __atomic_store_n(&session_page_tail->next, page, __ATOMIC_RELEASE);
        session_page_tail = page;
        __atomic_store_n(&session_page_count, page_count + 1u, __ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&session_page_lock, flags);
    }
}

/**
 * @brief Reclaim a hung-up PTY slot after the final endpoint and task reference closes.
 * @param pty_id Stable 1-based PTY ID to check.
 */
void pty_reap_hungup(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !session->hungup || session->console) {
        return;
    }
    if (sched_pty_reference_count(pty_id) != 0) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&session_page_lock, &flags);
    if (__atomic_load_n(&session->used, __ATOMIC_ACQUIRE) &&
        session->hungup && !session->console && !session->transfer_refs) {
        __builtin_memset(session, 0, sizeof(*session));
    }
    kernel_spin_unlock_irqrestore(&session_page_lock, flags);
}

int pty_transfer_get(uint32_t pty_id, uint32_t endpoint)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return -9;
    ++session->transfer_refs;
    if (endpoint == TASK_PTY_ENDPOINT_MASTER) ++session->transfer_masters;
    return 0;
}

uint32_t pty_transfer_count(uint32_t pty_id, int master_only)
{
    struct pty_session *session = find_session(pty_id);
    return session ? (master_only ? session->transfer_masters : session->transfer_refs) : 0;
}

void pty_transfer_put(uint32_t pty_id, uint32_t endpoint)
{
    struct pty_session *session = find_session(pty_id);
    if (!session) return;
    if (session->transfer_refs) --session->transfer_refs;
    if (endpoint == TASK_PTY_ENDPOINT_MASTER && session->transfer_masters) {
        --session->transfer_masters;
        if (!sched_pty_master_reference_count(pty_id))
            (void)pty_destroy(session->owner_pid, pty_id);
    }
    pty_reap_hungup(pty_id);
}

/**
 * @brief Close the master side of pty_id using Unix98 hangup semantics.
 *
 * The session is not freed while slave descriptors remain open. The slave
 * keeps draining buffered input, then reports EOF/POLLHUP and rejects writes
 * with EIO. SIGHUP and SIGCONT are delivered to the controlling session leader.
 */
int pty_destroy(uint32_t owner_pid, uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || pty_vt_number(pty_id) || session->owner_pid != owner_pid) {
        return -22;
    }
    if (session->hungup) {
        return 0;
    }
    /* Preserve bytes typed before the master closed.  Canonical input is
     * held separately until a delimiter arrives, but a slave still needs to
     * drain that pending line before observing EOF after hangup. */
    pty_commit_canonical_input(session);
    session->hungup = 1;
    session->owner_pid = 0;
    uint32_t sid = session->process_session;
    struct task *leader = sid ? sched_find(sid) : NULL;
    if (leader) leader->tty_old_pgrp = session->foreground_pgid;
    session->process_session = 0;
    session->foreground_pgid = 0;
    (void)sched_hangup_user_tasks_for_pty(pty_id, owner_pid);
    if (sid) {
        sched_signal_user_process(sid, 1);
        sched_signal_user_process(sid, 18);
    }
    kernel_poll_readiness_notify();
    pty_reap_hungup(pty_id);
    return 0;
}

/**
 * @brief Return 1 if owner_pid owns pty_id.
 */
int pty_is_owner(uint32_t pty_id, uint32_t owner_pid)
{
    struct pty_session *session = find_session(pty_id);
    return session && session->owner_pid == owner_pid;
}

/**
 * @brief Return 1 if pty_id is an allocated session.
 */
int pty_is_active(uint32_t pty_id)
{
    return find_session(pty_id) != 0;
}

int pty_is_hungup(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    return session && session->hungup;
}

int pty_slave_open_allowed(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    return session && !session->locked && !session->hungup;
}

int pty_set_lock(uint32_t pty_id, int locked)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || session->hungup) return -5;
    session->locked = locked ? 1u : 0u;
    return 0;
}

int pty_get_lock(uint32_t pty_id, int *locked)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !locked) return -22;
    *locked = session->locked ? 1 : 0;
    return 0;
}

#include "internal.h"


struct pty_session sessions[PTY_MAX];

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
 * @brief Return the PTY session for pty_id (1-based), or NULL if invalid/unused.
 */
struct pty_session *find_session(uint32_t pty_id)
{
    if (pty_id == 0 || pty_id > PTY_MAX) {
        return 0;
    }
    if (!sessions[pty_id - 1].used) {
        return 0;
    }
    return &sessions[pty_id - 1];
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
        id = id * 10 + (uint32_t)(*path++ - '0');
        if (id > PTY_MAX) return -2;
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
 * @brief Zero all PTY sessions.
 */
void pty_init(void)
{
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
    for (uint32_t i = 0; i < PTY_MAX; ++i) {
        for (uint32_t j = 0; j < sizeof(sessions[i]); ++j) {
            ((uint8_t *)&sessions[i])[j] = 0;
        }
    }
}

/**
 * @brief Allocate and initialize a PTY for owner_pid with default termios/winsize; returns its id, or -12 when exhausted.
 */
int32_t pty_create(uint32_t owner_pid)
{
    for (uint32_t i = 0; i < PTY_MAX; ++i) {
        if (sessions[i].used && sessions[i].hungup &&
            !sessions[i].console &&
            sched_pty_reference_count(i + 1U) == 0) {
            for (uint32_t j = 0; j < sizeof(sessions[i]); ++j) {
                ((uint8_t *)&sessions[i])[j] = 0;
            }
        }
    }
    for (uint32_t i = 0; i < PTY_MAX; ++i) {
        if (!sessions[i].used) {
            sessions[i].used = 1;
            /* Linux devpts keeps the slave inaccessible until unlockpt(). */
            sessions[i].locked = 1;
            sessions[i].owner_pid = owner_pid;
            const struct task *owner = sched_find(owner_pid);
            sessions[i].permissions = (struct reliefos_permissions){0600,
                owner ? owner->fsuid : 0, owner ? owner->fsgid : 0};
            if (!++next_generation) ++next_generation;
            sessions[i].generation = next_generation;
            /* Opening ptmx does not acquire a controlling terminal. */
            sessions[i].process_session = 0;
            sessions[i].foreground_pgid = 0;
            sessions[i].input_head = 0;
            sessions[i].input_tail = 0;
            sessions[i].output_head = 0;
            sessions[i].output_tail = 0;
            sessions[i].termios.c_iflag = RELIEFOS_PTY_IFLAG_ICRNL;
            sessions[i].termios.c_oflag = LINUX_OPOST | LINUX_ONLCR;
            sessions[i].termios.c_cflag = LINUX_CLOCAL | LINUX_CREAD |
                                         LINUX_CS8 | LINUX_B115200;
            sessions[i].termios.c_lflag = RELIEFOS_PTY_LFLAG_ECHO |
                                          RELIEFOS_PTY_LFLAG_ECHONL |
                                          RELIEFOS_PTY_LFLAG_ICANON |
                                          RELIEFOS_PTY_LFLAG_IEXTEN |
                                          RELIEFOS_PTY_LFLAG_ISIG;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VEOF] = 4;     /* Ctrl-D */
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VEOL] = 0;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VERASE] = 127; /* DEL */
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VINTR] = 3;    /* Ctrl-C */
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VKILL] = 21;   /* Ctrl-U */
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VMIN] = 1;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VQUIT] = 28;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VSTART] = 17;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VSTOP] = 19;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VSUSP] = 26;
            sessions[i].termios.c_cc[RELIEFOS_PTY_CC_VTIME] = 0;
            sessions[i].termios.c_ispeed = 115200U;
            sessions[i].termios.c_ospeed = 115200U;
            {
                const struct framebuffer *fb = framebuffer_get();
                uint32_t cols = fb && fb->available ? fb->width / RELIEFOS_FONT_W : 80u;
                uint32_t rows = fb && fb->available ? fb->height / RELIEFOS_FONT_H : 24u;
                sessions[i].winsize.ws_row = (uint16_t)(rows > 0xffffu ? 0xffffu : rows);
                sessions[i].winsize.ws_col = (uint16_t)(cols > 0xffffu ? 0xffffu : cols);
                if (!sessions[i].winsize.ws_row) sessions[i].winsize.ws_row = 24;
                if (!sessions[i].winsize.ws_col) sessions[i].winsize.ws_col = 80;
            }
            console_debug("[reliefnt] pty create owner=%u id=%u session=%u pgrp=%u\n",
                           owner_pid, (uint32_t)(i + 1),
                           sessions[i].process_session,
                           sessions[i].foreground_pgid);
            return (int32_t)(i + 1);
        }
    }
    return -12;
}

/**
 * @brief Reclaim a hung-up PTY once no endpoint or controlling-TTY reference remains.
 */
void pty_reap_hungup(uint32_t pty_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !session->hungup || session->console) {
        return;
    }
    if (sched_pty_reference_count(pty_id) == 0) {
        for (uint32_t j = 0; j < sizeof(*session); ++j) {
            ((uint8_t *)session)[j] = 0;
        }
    }
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

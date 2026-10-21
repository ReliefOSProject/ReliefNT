#include "internal.h"



/**
 * @brief Gets the process group currently receiving terminal-generated signals.
 * @param pty_id PTY identifier.
 * @param process_group Destination for the foreground process-group identifier.
 * @return Zero on success or a negative errno-style failure.
 */
int pty_get_foreground_pgid(uint32_t pty_id, uint32_t *process_group)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !process_group) {
        return -22;
    }
    *process_group = session->foreground_pgid;
    return 0;
}

/**
 * @brief Changes the group that owns foreground terminal input.
 * @param pty_id PTY identifier.
 * @param caller_pid Attached process requesting the change.
 * @param process_group New foreground process-group identifier.
 * @return Zero on success or a negative errno-style failure.
 */
int pty_set_foreground_pgid(uint32_t pty_id, uint32_t caller_pid,
                            uint32_t process_group)
{
    struct pty_session *session = find_session(pty_id);
    struct task *caller = sched_find(caller_pid);
    if ((int32_t)process_group < 0) return -22;
    if (!session || !caller || caller->controlling_pty_id != pty_id ||
        caller->process_session != session->process_session) return -25;
    int64_t group_session = sched_process_group_session(process_group);
    if (group_session < 0) return (int)group_session;
    if ((uint32_t)group_session != caller->process_session) return -1;
    session->foreground_pgid = process_group;
    return 0;
}

/**
 * @brief Apply Linux TIOCSCTTY session-leader, read access and steal rules.
 * @param pty_id Active terminal ID.
 * @param caller_pid Calling thread ID.
 * @param steal Raw ioctl argument, whose value 1 requests privileged stealing.
 * @param readable Whether the descriptor was opened for reading.
 * @return Zero on attachment or a negative Linux errno.
 */
int pty_acquire_controlling(uint32_t pty_id, uint32_t caller_pid, int steal, int readable)
{
    struct pty_session *session = find_session(pty_id);
    struct task *caller = sched_find(caller_pid);
    if (!session || !caller) return -25;
    uint32_t leader = caller->tgid ? caller->tgid : caller->pid;
    if (caller->process_session == leader && session->process_session == leader)
        return 0;
    if (caller->process_session != leader || caller->controlling_pty_id)
        return -1;
    if (session->process_session) {
        if (steal != 1 || !(caller->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -1;
        sched_clear_controlling_pty(pty_id);
    }
    if (!readable && !(caller->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -1;
    session->process_session = caller->process_session;
    session->foreground_pgid = caller->process_group;
    struct task *process = sched_find(leader);
    if (process) process->tty_old_pgrp = 0;
    sched_set_controlling_pty(caller_pid, pty_id);
    return 0;
}

int pty_get_session(uint32_t pty_id, uint32_t *session_id)
{
    struct pty_session *session = find_session(pty_id);
    if (!session || !session->process_session) return -25;
    *session_id = session->process_session;
    return 0;
}

void pty_open_controlling(uint32_t pty_id, uint32_t caller_pid, int readable)
{
    struct pty_session *session = find_session(pty_id);
    if (session && !session->process_session && readable)
        (void)pty_acquire_controlling(pty_id, caller_pid, 0, 1);
}

int pty_detach_controlling(uint32_t pty_id, uint32_t caller_pid)
{
    struct pty_session *session = find_session(pty_id);
    struct task *caller = sched_find(caller_pid);
    if (!session || !caller || caller->controlling_pty_id != pty_id) return -25;
    uint32_t leader = caller->tgid ? caller->tgid : caller->pid;
    if (caller->process_session == leader) {
        uint32_t group = session->foreground_pgid;
        sched_clear_controlling_pty(pty_id);
        session->process_session = 0;
        session->foreground_pgid = 0;
        if (pty_vt_number(pty_id) &&
            (!vt_controller_pid[pty_id - 1u] || vt_controller_pid[pty_id - 1u] == leader)) {
            (void)pty_vt_set_graphics(pty_id, 0);
            (void)pty_vt_reset_mode(pty_id);
        }
        if (group) {
            sched_signal_kernel_group(group, 1);
            sched_signal_kernel_group(group, 18);
        }
    } else sched_set_controlling_pty(caller_pid, 0);
    return 0;
}

void pty_process_session_exit(uint32_t tgid)
{
    /* The last-thread exit hook runs for every TGID, not just session leaders. */
    for (uint32_t i = 0; i < VT_COUNT; ++i) {
        if (vt_modes[i].mode == VT_PROCESS && vt_controller_pid[i] == tgid)
            (void)pty_vt_reset_mode(i + 1u);
    }
    struct task *leader = sched_find(tgid);
    if (!leader || leader->process_session != tgid) return;
    uint32_t old = leader->tty_old_pgrp;
    leader->tty_old_pgrp = 0;
    struct pty_session *session = find_session(leader->controlling_pty_id);
    if (session) {
        uint32_t pty_id = leader->controlling_pty_id;
        uint32_t foreground = session->foreground_pgid;
        sched_clear_controlling_pty(pty_id);
        session->process_session = 0;
        session->foreground_pgid = 0;
        if (pty_vt_number(pty_id) &&
            (!vt_controller_pid[pty_id - 1u] || vt_controller_pid[pty_id - 1u] == tgid)) {
            (void)pty_vt_set_graphics(pty_id, 0);
            (void)pty_vt_reset_mode(pty_id);
        }
        if (foreground) sched_signal_kernel_group(foreground, 1);
    } else if (old) {
        sched_signal_kernel_group(old, 1);
        sched_signal_kernel_group(old, 18);
    }
}

int64_t pty_check_change(uint32_t pty_id, uint32_t caller_pid, int signal_number)
{
    struct pty_session *session = find_session(pty_id);
    struct task *caller = sched_find(caller_pid);
    if (!session || !caller || caller->controlling_pty_id != pty_id ||
        !session->foreground_pgid || session->foreground_pgid == caller->process_group)
        return 0;
    if (!signal_number) {
        if (!(session->termios.c_lflag & LINUX_TOSTOP)) return 0;
        signal_number = 22;
    }
    uint64_t bit = 1ULL << (signal_number - 1);
    if ((caller->blocked_signals & bit) || sched_task_actions(caller)[signal_number].handler == 1)
        return signal_number == 21 ? -5 : 0;
    if (sched_process_group_orphaned(caller->process_group)) return -5;
    sched_signal_kernel_group(caller->process_group, signal_number);
    return KERNEL_SYSCALL_BLOCKED;
}

/**
 * @brief Retire legacy owners without closing a retained Unix98 master.
 */
void pty_process_exit(uint32_t pid)
{
    for (uint32_t i = 0; i < PTY_MAX; ++i) {
        if (sessions[i].used && sessions[i].owner_pid == pid &&
            !sched_pty_master_reference_count(i + 1U)) {
            (void)pty_destroy(pid, i + 1U);
        }
    }
}

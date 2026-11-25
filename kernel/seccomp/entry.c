/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/sched.h>
#include <reliefnt/signal.h>
#include <reliefnt/time.h>
#include <linux/audit.h>
#include <linux/errno.h>

#define SECCOMP_MAX_ERRNO 4095u
#define SECCOMP_LOG_PER_SECOND 8u

/** @brief Emit bounded diagnostics without exposing user syscall arguments.
 * @param task Triggering execution-owned task. @param nr Native syscall number.
 * @param action Selected return action. This global budget is protected by the
 * same writer that admits filtered syscalls; there is no per-task allocation.
 */
static void seccomp_log(const struct task *task, int32_t nr, uint32_t action)
{
    static uint64_t since;
    static unsigned logged;
    uint64_t now = time_uptime_ms();
    if (now - since >= 1000u) {
        since = now;
        logged = 0;
    }
    if (logged >= SECCOMP_LOG_PER_SECOND) return;
    ++logged;
    console_info("[seccomp] tgid=%u tid=%u nr=%d action=0x%x\n",
                 sched_task_tgid(task), task->pid, nr, action);
}

/** @brief Terminate the caller or its thread group with wait-visible SIGSYS.
 * @param task Current user task under exclusive execution ownership.
 * @param whole_group True for a process-wide action, false for this thread.
 */
static void seccomp_terminate(struct task *task, bool whole_group)
{
    const uint64_t code = 128 + LINUX_SIGSYS;
    uint32_t tgid = sched_task_tgid(task);
    if (!whole_group) {
        /* The final member's fatal SIGSYS is visible to the parent even
         * when the waitable leader previously exited normally. */
        whole_group = true;
        struct task *other;
        for (uint32_t i = 0; (other = sched_task_at(i)); ++i) {
            if (other != task && other->pid && other->state != TASK_EXITED &&
                sched_task_tgid(other) == tgid) {
                whole_group = false;
                break;
            }
        }
    }
    task->exit_signal = LINUX_SIGSYS;
    if (whole_group) {
        struct task *leader = sched_find(tgid);
        if (leader) leader->exit_signal = LINUX_SIGSYS;
        sched_exit_group(tgid, code);
    } else {
        sched_exit(task->pid, code);
    }
    sched_notify_runnable();
}

/** @brief Enforce the strongest filter result over a complete register snapshot.
 * @param task Running user thread with a pinned, nonempty filter chain.
 * @param frame Borrowed native entry frame; modified for synthesized returns.
 * @return True to execute the call; false means no backend may run.
 * Preserves original RAX and post-instruction RIP for a caught x86-64 TRAP.
 */
bool seccomp_admit_syscall(struct task *task, struct trap_frame *frame)
{
    const struct seccomp_data data = {
        .nr = (int32_t)frame->rax,
        .arch = AUDIT_ARCH_X86_64,
        .instruction_pointer = frame->rip,
        .args = {frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9},
    };
    int32_t rank = (int32_t)SECCOMP_RET_ALLOW;
    uint32_t response = SECCOMP_RET_ALLOW;
    bool log = false;
    for (const struct seccomp_filter *filter = task->seccomp_filter;
         filter; filter = filter->older) {
        uint32_t current = seccomp_bpf_run(filter->code, filter->length, &data);
        int32_t next_rank = (int32_t)(current & SECCOMP_RET_ACTION_FULL);
        /* Compare only signed action bits. Equal ranks retain the newest data. */
        if (next_rank < rank) {
            rank = next_rank;
            response = current;
            log = filter->log;
        }
    }

    uint32_t action = response & SECCOMP_RET_ACTION_FULL;
    uint32_t payload = response & SECCOMP_RET_DATA;
    if (action == SECCOMP_RET_LOG || (log && action != SECCOMP_RET_ALLOW))
        seccomp_log(task, data.nr, action);
    switch (action) {
    case SECCOMP_RET_ALLOW:
    case SECCOMP_RET_LOG:
        return true;
    case SECCOMP_RET_ERRNO:
        if (payload > SECCOMP_MAX_ERRNO) payload = SECCOMP_MAX_ERRNO;
        frame->rax = (uint64_t)-(int64_t)payload;
        return false;
    case SECCOMP_RET_TRAP: {
        const uint64_t bit = UINT64_C(1) << (LINUX_SIGSYS - 1);
        if ((task->blocked_signals & bit) ||
            sched_task_actions(task)[LINUX_SIGSYS].handler <= 1) {
            seccomp_terminate(task, true);
        } else {
            task->sigwait_mask &= ~bit;
            kernel_signal_enqueue_seccomp(task, data.instruction_pointer, data.nr,
                                          (uint16_t)payload);
        }
        return false;
    }
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        /* No tracer mediation or notification listener is implemented. */
        frame->rax = (uint64_t)-(int64_t)LINUX_ENOSYS;
        return false;
    case SECCOMP_RET_KILL_THREAD:
        seccomp_terminate(task, false);
        return false;
    case SECCOMP_RET_KILL_PROCESS:
    default:
        seccomp_terminate(task, true);
        return false;
    }
}

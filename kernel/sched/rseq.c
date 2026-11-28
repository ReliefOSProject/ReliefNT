/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>
#include <linux/rseq.h>
#include <linux/errno.h>

/**
 * @brief Validate a userspace rseq descriptor and discard its active pointer.
 * @param task User task with a registered rseq area.
 * @param frame Registers interrupted by preemption or signal delivery.
 * @return Zero, EINVAL for a malformed descriptor, or EFAULT for inaccessible data.
 * Called under execution ownership, after releasing scheduler_lock.
 */
static int rseq_fixup(struct task *task, struct trap_frame *frame)
{
    struct rseq area;
    struct rseq_cs section;
    uint64_t zero = 0;
    uint32_t signature;
    if (user_copy_from_task(task, &area, task->rseq_area, sizeof(area)) < 0)
        return -LINUX_EFAULT;
    if (!area.rseq_cs) return 0;
    if (user_copy_from_task(task, &section, area.rseq_cs, sizeof(section)) < 0)
        return -LINUX_EFAULT;
    if (section.version || section.start_ip >= RELIEFNT_USER_TOP ||
        section.post_commit_offset >= RELIEFNT_USER_TOP - section.start_ip ||
        section.abort_ip < sizeof(signature) || section.abort_ip >= RELIEFNT_USER_TOP ||
        section.abort_ip - section.start_ip < section.post_commit_offset)
        return -LINUX_EINVAL;
    if (user_copy_from_task(task, &signature, section.abort_ip - sizeof(signature),
                             sizeof(signature)) < 0)
        return -LINUX_EFAULT;
    if (signature != task->rseq_sig) return -LINUX_EINVAL;
    bool active = frame->rip - section.start_ip < section.post_commit_offset;
    if (active && (area.flags || section.flags)) return -LINUX_EINVAL;
    if (user_copy_to_task(task, task->rseq_area + offsetof(struct rseq, rseq_cs),
                           &zero, sizeof(zero)) < 0)
        return -LINUX_EFAULT;
    if (active) frame->rip = section.abort_ip;
    return 0;
}

/**
 * @brief Handle the rseq notification before entering a user context.
 * @param task Pinned user task, with no scheduler spinlock held.
 * @param frame Live or saved register frame.
 * @param cpu CPU that owns the return-to-user context.
 * @param fixup Whether preemption or a signal interrupted userspace.
 * @return Zero or a negative descriptor/usercopy error.
 */
int sched_rseq_notify(struct task *task, struct trap_frame *frame,
                       uint32_t cpu, bool fixup)
{
    if (!task || task->kind != TASK_KIND_USER || !task->rseq_area) return 0;
    if (fixup) {
        int result = rseq_fixup(task, frame);
        if (result < 0) return result;
    }
    /* CPU indices provide per-mm concurrency IDs unique among executing
     * threads. Preemption aborts sections before an index is reused. */
    uint32_t zero = 0;
    if (user_copy_to_task(task, task->rseq_area + offsetof(struct rseq, cpu_id_start),
                           &cpu, sizeof(cpu)) < 0 ||
        user_copy_to_task(task, task->rseq_area + offsetof(struct rseq, cpu_id),
                           &cpu, sizeof(cpu)) < 0 ||
        user_copy_to_task(task, task->rseq_area + offsetof(struct rseq, node_id),
                           &zero, sizeof(zero)) < 0 ||
        user_copy_to_task(task, task->rseq_area + offsetof(struct rseq, mm_cid),
                           &cpu, sizeof(cpu)) < 0)
        return -LINUX_EFAULT;
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/sched.h>
#include <linux/errno.h>
#include <linux/rseq.h>

static struct rseq area __attribute__((aligned(32)));
static struct rseq_cs section __attribute__((aligned(32)));
static uint32_t abort_code[2] = {0x53053053U, 0};
static uint64_t inaccessible;

/** @brief Model exact task-aware copies without dereferencing invalid addresses. */
int user_copy_from_task(struct task *task, void *out, uint64_t address, uint64_t size)
{
    (void)task;
    if (!address || address == inaccessible) return -LINUX_EFAULT;
    memcpy(out, (const void *)(uintptr_t)address, size);
    return 0;
}
int user_copy_to_task(struct task *task, uint64_t address, const void *data, uint64_t size)
{
    (void)task;
    if (!address || address == inaccessible) return -LINUX_EFAULT;
    memcpy((void *)(uintptr_t)address, data, size);
    return 0;
}

int main(void)
{
    struct task task = {.kind = TASK_KIND_USER, .rseq_area = (uintptr_t)&area,
                        .rseq_sig = abort_code[0]};
    struct trap_frame frame = {.rip = 0x400005};
    section.start_ip = 0x400000;
    section.post_commit_offset = 16;
    section.abort_ip = (uintptr_t)&abort_code[1];
    area.rseq_cs = (uintptr_t)&section;
    assert(!sched_rseq_notify(&task, &frame, 3, false));
    assert(frame.rip == 0x400005 && area.rseq_cs == (uintptr_t)&section);
    assert(area.cpu_id_start == 3 && area.cpu_id == 3 && area.mm_cid == 3);
    assert(!sched_rseq_notify(&task, &frame, 1, true));
    assert(frame.rip == section.abort_ip && !area.rseq_cs);
    assert(area.cpu_id_start == 1 && area.cpu_id == 1 && area.mm_cid == 1);
    frame.rip = section.start_ip + section.post_commit_offset;
    area.rseq_cs = (uintptr_t)&section;
    assert(!sched_rseq_notify(&task, &frame, 0, true));
    assert(frame.rip == 0x400010 && !area.rseq_cs);

    /* Even outside the section, descriptors and abort signatures are checked. */
    area.rseq_cs = (uintptr_t)&section;
    section.version = 1;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    section.version = 0;
    abort_code[0] ^= 1;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    abort_code[0] ^= 1;
    frame.rip = section.start_ip;
    section.flags = 1;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    section.flags = 0;
    area.flags = 1;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    area.flags = 0;
    section.abort_ip = section.start_ip + 4;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    section.abort_ip = (uintptr_t)&abort_code[1];
    section.post_commit_offset = RELIEFNT_USER_TOP;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EINVAL);
    section.post_commit_offset = 16;
    inaccessible = (uintptr_t)&section;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EFAULT);
    inaccessible = (uintptr_t)&area;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EFAULT);
    inaccessible = (uintptr_t)&area.cpu_id;
    area.rseq_cs = 0;
    assert(sched_rseq_notify(&task, &frame, 0, true) == -LINUX_EFAULT);
    task.rseq_area = 0;
    assert(!sched_rseq_notify(&task, &frame, 0, true));
    puts("PASS rseq: CPU/mm_cid publication, preemption abort, syscall preservation and descriptor faults");
    return 0;
}

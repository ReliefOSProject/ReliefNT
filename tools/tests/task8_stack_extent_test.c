/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: stack extent.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/mm.h>
#include <reliefnt/sched.h>
#include <linux/resource.h>
static uint64_t used;
uint64_t task_vma_total_bytes(const struct task *task){(void)task;return used;}
int main(void)
{
    struct task task={0};uint64_t end=0;
    task.kind=TASK_KIND_USER;task.stack_top=0x70000000;task.stack_low=task.stack_top-8192;
    task.limits.stack.rlim_cur=16384;task.limits.as.rlim_cur=LINUX_RLIM_INFINITY;
    assert(mm_user_stack_readable_extent(&task,task.stack_top-16384,&end) && end==task.stack_top);
    assert(!mm_user_stack_readable_extent(&task,task.stack_top-20480,&end));
    assert(mm_user_stack_readable_extent(&task,task.stack_low,&end));
    task.limits.stack.rlim_cur=LINUX_RLIM_INFINITY;task.limits.as.rlim_cur=16384;used=12288;
    assert(mm_user_stack_readable_extent(&task,task.stack_low-4096,&end));
    assert(!mm_user_stack_readable_extent(&task,task.stack_low-8192,&end));
    assert(!mm_user_stack_readable_extent(&task,task.stack_top,&end));
    puts("PASS real stack eligibility: existing interval, stack/AS limits and top boundary");
    return 0;
}

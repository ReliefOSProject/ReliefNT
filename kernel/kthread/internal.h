/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_KTHREAD_INTERNAL_H
#define RELIEFNT_KTHREAD_INTERNAL_H
#include <reliefnt/kthread.h>
#include <reliefnt/sched.h>
#include <reliefnt/sched_kernel.h>
#include <reliefnt/lock.h>
#include <reliefnt/module.h>
#include <reliefnt/smp.h>
#include <reliefnt/time.h>
#include <linux/errno.h>
struct kthread {
    struct kthread *all_next,*ready_next;
    kthread_fn function;
    void *context,*stack;
    struct module *owner;
    struct task *task;
    uint64_t rsp,deadline;
    uint32_t cpu;
    int result;
    bool queued,running,started,admitted,wake_pending,stop,finished,exited,closed;
};
extern struct kernel_spinlock kthread_lock;
extern struct kthread *kthread_all,*kthread_ready[SMP_MAX_CPUS],*kthread_tail[SMP_MAX_CPUS];
extern struct kthread *kthread_active[SMP_MAX_CPUS];
extern uint32_t kthread_ready_count[SMP_MAX_CPUS];
extern uint64_t kthread_dispatch_sp[SMP_MAX_CPUS];
/** @brief Append a stopped, non-running context to its bound CPU FIFO.
 * @param thread Stable handle. kthread_lock held, IRQs disabled. */
void kthread_enqueue_locked(struct kthread *thread);
/** @brief Unlink a queued context without changing its execution pin.
 * @param thread Stable handle. kthread_lock held, IRQs disabled. */
void kthread_unqueue_locked(struct kthread *thread);
/** @brief Save one kernel continuation and load another, with IRQs disabled.
 * @param old Writable RSP slot. @param next Stable source RSP slot. */
void arch_kthread_switch(uint64_t *old,const uint64_t *next);
/** @brief Enable IRQs and halt atomically, returning with IRQs disabled. */
void arch_kthread_idle(void);
/** @brief Enter a newly dispatched thread and park forever after final return.
 * Called by its initial stack; core text remains mapped after module return. */
__attribute__((noreturn)) void kthread_entry(void);
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/storage_wait.h>
#include <reliefnt/export.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/wait.h>

/* Ordering: event_lock -> waiters.lock -> scheduler lock. IRQ publishers never
 * take the filesystem execution writer, transport lock or a controller lock. */
static struct kernel_spinlock event_lock=KERNEL_SPINLOCK_INIT;
static struct kernel_wait_queue waiters={.lock=KERNEL_SPINLOCK_INIT};
static uint64_t generation;
/* Per execution transaction, never accessed by an interrupt. */
static uint64_t wait_deadline;

/** @brief Snapshot completion publication before a serialized syscall checks I/O.
 * @return Current generation. Removes this task's expired storage wait only.
 */
uint64_t storage_wait_begin(void)
{
    kernel_wait_queue_remove(&waiters,sched_current_task());
    wait_deadline=0;
    return __atomic_load_n(&generation,__ATOMIC_ACQUIRE);
}

/** @brief Retain the earliest hardware timeout requested by this transaction.
 * @param deadline Nonzero absolute tick. Execution writer held, no scheduling.
 */
void storage_wait_hint(uint64_t deadline)
{
    if(deadline && (!wait_deadline || deadline<wait_deadline))wait_deadline=deadline;
}

/** @brief Advance the condition epoch and wake all registered storage waiters.
 * Hardware completion or retired global storage ownership counts as progress.
 * IRQ/task safe; no driver lock, allocation, I/O or execution writer required.
 */
void storage_completion_notify(void)
{
    uint64_t flags;kernel_spin_lock_irqsave(&event_lock,&flags);
    __atomic_add_fetch(&generation,1,__ATOMIC_RELEASE);
    kernel_wait_queue_wake_all(&waiters);
    kernel_spin_unlock_irqrestore(&event_lock,flags);
}

/** @brief Snapshot storage progress without acquiring the completion lock.
 * @return Monotonic completion/ownership-retirement generation.
 */
uint64_t storage_completion_generation(void)
{
    return __atomic_load_n(&generation,__ATOMIC_ACQUIRE);
}

/** @brief Atomically check the completion epoch and publish a syscall wait.
 * @param observed Snapshot taken before checking the provider's readiness.
 * Caller has unwound every driver/transport lock; no C stack is suspended.
 */
void storage_wait_sleep(uint64_t observed)
{
    uint64_t flags;kernel_spin_lock_irqsave(&event_lock,&flags);
    if(__atomic_load_n(&generation,__ATOMIC_ACQUIRE)==observed) {
        uint64_t now=time_ticks(),deadline=wait_deadline;
        if(!deadline)deadline=now+1;
        if(deadline>now && kernel_wait_queue_sleep_current_until(&waiters,deadline))
            sched_sleep_current_until(now+1);
    }
    kernel_spin_unlock_irqrestore(&event_lock,flags);
}
EXPORT_SYMBOL(storage_wait_hint);
EXPORT_SYMBOL(storage_completion_notify);

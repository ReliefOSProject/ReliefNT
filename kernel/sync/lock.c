/*
 * ReliefOS kernel synchronization implementation.
 * Implements compact x86_64 spin locks and interrupt state preservation.
 */
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>

/* The execution lock may be entered again while the owning CPU resolves a
 * kernel-mode user-memory fault.  A plain spin lock would self-deadlock in
 * that path, so retain a small per-CPU recursion count.  The owner is only
 * inspected lock-free by the owning CPU. A ticket lock makes the global
 * kernel-service transaction FIFO: a new scheduler pass cannot repeatedly
 * steal it from a pending exec or page-in on another CPU. */
static uint32_t execution_next_ticket;
static uint32_t execution_serving_ticket;
static uint32_t execution_owner = UINT32_MAX;
static uint32_t execution_readers;
static uint32_t execution_depth[SMP_MAX_CPUS];
static uint64_t execution_outer_flags[SMP_MAX_CPUS];

static uint32_t execution_cpu_index(void)
{
    uint32_t cpu = smp_current_cpu();
    return cpu < SMP_MAX_CPUS ? cpu : 0;
}

void kernel_spin_init(struct kernel_spinlock *lock)
{
    if (lock) {
        lock->state = 0;
    }
}

void kernel_spin_lock(struct kernel_spinlock *lock)
{
    uint32_t busy = 1;
    if (!lock) {
        return;
    }
    for (;;) {
        __asm__ volatile("xchg %0, %1" : "+r"(busy), "+m"(lock->state)
                         : : "memory");
        if (busy == 0) {
            return;
        }
        busy = 1;
        smp_membarrier_poll();
        __asm__ volatile("pause" : : : "memory");
    }
}

void kernel_spin_unlock(struct kernel_spinlock *lock)
{
    if (lock) {
        __asm__ volatile("movl $0, %0" : "=m"(lock->state) : : "memory");
    }
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    uint64_t saved = kernel_irq_save();
    if (flags) {
        *flags = saved;
    }
    kernel_spin_lock(lock);
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    kernel_spin_unlock(lock);
    kernel_irq_restore(flags);
}

void kernel_execution_lock_irqsave(uint64_t *flags)
{
    /* Recursion belongs to an execution context, not merely to a CPU. A
     * timer-driven task switch while this lock is held would otherwise let
     * the newly scheduled task on the same CPU enter the protected region as
     * if it were the owner.  Keep local interrupts disabled for the bounded
     * kernel transaction so ownership cannot cross a scheduler boundary.
     * FIFO admission prevents the high-frequency started-task scheduler path
     * from starving a pending image load on another CPU. */
    uint64_t saved = kernel_irq_save();
    if (flags) {
        *flags = saved;
    }
    uint32_t cpu = execution_cpu_index();
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] != 0) {
        ++execution_depth[cpu];
        return;
    }
    uint32_t ticket = __atomic_fetch_add(&execution_next_ticket, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&execution_serving_ticket, __ATOMIC_SEQ_CST) != ticket ||
           __atomic_load_n(&execution_readers, __ATOMIC_SEQ_CST) != 0) {
        /* IRQs are masked here. A membarrier owner must still be able to
         * rendezvous with CPUs waiting for this lock. */
        smp_membarrier_poll();
        __asm__ volatile("pause" : : : "memory");
    }
    __atomic_store_n(&execution_owner, cpu, __ATOMIC_RELAXED);
    execution_depth[cpu] = 1;
    execution_outer_flags[cpu] = saved;
}

/** @brief Release one recursive execution ownership level and restore IRQ state.
 * @param flags IRQ flags captured by the matching lock call; a final outer unlock
 * restores the outer transaction state saved by the lock or suspend token.
 * @return None. Must be called by the current execution owner.
 */
void kernel_execution_unlock_irqrestore(uint64_t flags)
{
    uint32_t cpu = execution_cpu_index();
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] > 1) {
        --execution_depth[cpu];
        kernel_irq_restore(flags);
        return;
    }
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] == 1) {
        uint64_t outer_flags = execution_outer_flags[cpu];
        execution_depth[cpu] = 0;
        execution_outer_flags[cpu] = 0;
        __atomic_store_n(&execution_owner, UINT32_MAX, __ATOMIC_RELAXED);
        __atomic_fetch_add(&execution_serving_ticket, 1, __ATOMIC_SEQ_CST);
        kernel_irq_restore(outer_flags);
        return;
    }
    kernel_irq_restore(flags);
}

/**
 * @brief Release only an outermost execution owner while preserving its final IRQ restore state.
 * @param token Zero-initialized storage receiving the transaction, owner CPU and IRQ states.
 * @return 0 when suspended, -22 for bad input, -1 without current ownership, or -35 for
 * nested ownership. Errors restore the pre-call IRQ state and do not change ownership.
 */
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token)
{
    if (!token || token->active) {
        return -22;
    }
    uint64_t saved = kernel_irq_save();
    uint32_t cpu = execution_cpu_index();
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) != cpu) {
        kernel_irq_restore(saved);
        return -1;
    }
    if (execution_depth[cpu] != 1) {
        kernel_irq_restore(saved);
        return -35;
    }

    token->original_irq_flags = execution_outer_flags[cpu];
    token->wait_irq_flags = saved | (1ULL << 9);
    token->wait_exit_irq_flags = 0;
    token->suspended_cpu = cpu;
    token->active = 1;
    execution_depth[cpu] = 0;
    execution_outer_flags[cpu] = 0;
    __atomic_store_n(&execution_owner, UINT32_MAX, __ATOMIC_RELAXED);
    __atomic_fetch_add(&execution_serving_ticket, 1, __ATOMIC_SEQ_CST);

    /* Enable task-phase timer/MSI progress only after ownership is fully released. */
    kernel_irq_restore(token->wait_irq_flags);
    return 0;
}

/**
 * @brief Reacquire a suspended execution transaction without enabling IRQs under ownership.
 * @param token Active suspension token; successful resume consumes it.
 * @return 0 with execution ownership restored, -22 for an inactive token, or -35 if
 * the current context already has execution ownership. Failure preserves the active token.
 */
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token)
{
    if (!token || !token->active) {
        return -22;
    }
    uint64_t saved = kernel_irq_save();
    token->wait_exit_irq_flags = saved;
    uint32_t cpu = execution_cpu_index();
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu ||
        execution_depth[cpu] != 0) {
        kernel_irq_restore(saved);
        return -35;
    }

    uint64_t resume_flags;
    kernel_execution_lock_irqsave(&resume_flags);
    (void)resume_flags;
    execution_outer_flags[cpu] = token->original_irq_flags;
    token->active = 0;
    return 0;
}

/* Readers only mutate their independently owned address space. Writers still
 * exclude every reader, protecting VMA lifetime, fork, remote memory access,
 * and the legacy VFS/device scratch state. Never upgrade a read transaction:
 * release it and retry under the exclusive lock instead.
 *
 * Register then recheck admission. Sequential consistency ensures either the
 * writer observes this reader, or the reader observes the queued writer and
 * backs out before accessing protected data. Queuing a writer closes admission
 * immediately, so a stream of faults cannot starve an exclusive transaction. */
bool kernel_execution_try_read_lock_irqsave(uint64_t *flags)
{
    uint64_t saved = kernel_irq_save();
    if (flags) *flags = saved;
    if (__atomic_load_n(&execution_next_ticket, __ATOMIC_SEQ_CST) !=
        __atomic_load_n(&execution_serving_ticket, __ATOMIC_SEQ_CST)) {
        kernel_irq_restore(saved);
        return false;
    }
    __atomic_fetch_add(&execution_readers, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&execution_next_ticket, __ATOMIC_SEQ_CST) !=
        __atomic_load_n(&execution_serving_ticket, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&execution_readers, 1, __ATOMIC_SEQ_CST);
        kernel_irq_restore(saved);
        return false;
    }
    return true;
}

void kernel_execution_read_unlock_irqrestore(uint64_t flags)
{
    __atomic_fetch_sub(&execution_readers, 1, __ATOMIC_SEQ_CST);
    kernel_irq_restore(flags);
}

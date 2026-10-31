/*
 * ReliefOS kernel synchronization implementation.
 * Implements compact x86_64 spin locks and interrupt state preservation.
 */
#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include <reliefnt/lock_profile.h>
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

/** @brief Clamp the CPU index used by the existing gate state arrays.
 * @param cpu Raw CPU index returned by the platform, possibly out of range.
 * @return The index or zero as the gate's established fallback. No state,
 * allocation or IRQ changes; callers may retain the raw ID for profiling. */
static uint32_t execution_cpu_index(uint32_t cpu)
{
    return cpu < SMP_MAX_CPUS ? cpu : 0;
}

/** @brief Initialize an exclusively owned, unpublished lock to unlocked.
 * @param lock Caller-owned lock storage; NULL is ignored. No wait/allocation;
 * must not reset an active lock or one reachable by another CPU/IRQ.
 */
void kernel_spin_init(struct kernel_spinlock *lock)
{
    if (lock) {
        lock->state = 0;
    }
}

/** @brief Acquire CPU-wide exclusion while polling required TLB progress.
 * @param lock Stable shared lock, nullable no-op. Nonrecursive; caller controls
 * IRQ exclusion and lock order, never waits on an IRQ that needs this lock.
 * No scheduling/allocation or ownership transfer; release on the same path.
 */
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

/** @brief Publish prior protected accesses before relinquishing lock ownership.
 * @param lock Stable lock owned by this execution context, NULL accepted.
 * No wait/allocation/IRQ change; does not release unrelated lifetime references.
 */
void kernel_spin_unlock(struct kernel_spinlock *lock)
{
    if (lock) {
        __asm__ volatile("movl $0, %0" : "=m"(lock->state) : : "memory");
    }
}

/** @brief Disable local interrupts and acquire one nonrecursive spinlock.
 * @param lock Stable shared lock, nullable no-op. @param flags Optional saved IRQ output.
 * IRQ/task safe; preserves the caller's original IRQ state and never schedules
 * or allocates. Caller controls lock order, holds no recursive instance and
 * releases on the same context with kernel_spin_unlock_irqrestore.
 */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    uint64_t saved = kernel_irq_save();
    if (flags) {
        *flags = saved;
    }
    kernel_spin_lock(lock);
}

/** @brief Release a spinlock before restoring the matching caller IRQ state.
 * @param lock Borrowed lock held by this context, nullable.
 * @param flags Exact IRQ state from its matching acquisition.
 * IRQ/task safe, no allocation, wait or resource-reference transfer. Restoring
 * enabled IRQs occurs only after protected accesses have been published.
 */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    kernel_spin_unlock(lock);
    kernel_irq_restore(flags);
}

#ifdef CONFIG_LOCK_STAT
/** @brief Acquire recursive FIFO execution ownership and label its outer scope.
 * @param flags Optional writable original IRQ-state output, paired with unlock.
 * @param scope Fixed profiling ID; an invalid ID drops only the measurement.
 * Boot/task context; spins with IRQs masked, polls membarrier and excludes
 * readers. Recursion is same-context only; never hold over a scheduler wait. */
void kernel_execution_lock_scoped_irqsave(uint64_t *flags, uint32_t scope)
#else
/** @brief Acquire recursive FIFO execution ownership with local IRQs masked.
 * @param flags Optional writable original IRQ-state output, paired with unlock.
 * Boot/task context; spins with IRQs masked, polls membarrier and excludes
 * readers. Recursion is same-context only; never hold over a scheduler wait. */
void kernel_execution_lock_irqsave(uint64_t *flags)
#endif
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
    uint32_t raw_cpu = smp_current_cpu();
    uint32_t cpu = execution_cpu_index(raw_cpu);
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] != 0) {
        ++execution_depth[cpu];
        return;
    }
#ifdef CONFIG_LOCK_STAT
    struct lock_profile_sample sample;
    lock_profile_begin(&sample, raw_cpu, scope);
    bool contended = false;
#endif
    uint32_t ticket = __atomic_fetch_add(&execution_next_ticket, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&execution_serving_ticket, __ATOMIC_SEQ_CST) != ticket ||
           __atomic_load_n(&execution_readers, __ATOMIC_SEQ_CST) != 0) {
        /* IRQs are masked here. A membarrier owner must still be able to
         * rendezvous with CPUs waiting for this lock. */
#ifdef CONFIG_LOCK_STAT
        contended = true;
#endif
        smp_membarrier_poll();
        __asm__ volatile("pause" : : : "memory");
    }
#ifdef CONFIG_LOCK_STAT
    lock_profile_acquired(&sample, contended);
#endif
    __atomic_store_n(&execution_owner, cpu, __ATOMIC_RELAXED);
    execution_depth[cpu] = 1;
    execution_outer_flags[cpu] = saved;
}

#ifdef CONFIG_LOCK_STAT
/** @brief Acquire the exported FIFO gate with generic profiling attribution.
 * @param flags Optional writable original IRQ-state output, paired with unlock.
 * Boot/task context; spins with IRQs masked, allows same-context recursion and
 * excludes readers. Never retain ownership across a scheduler wait. */
void kernel_execution_lock_irqsave(uint64_t *flags)
{
    kernel_execution_lock_scoped_irqsave(flags, LOCK_SCOPE_GENERIC);
}
#endif

/** @brief Release one recursive execution ownership level and restore IRQ state.
 * @param flags IRQ flags captured by the matching lock call; a final outer unlock
 * restores the outer transaction state saved by the lock or suspend token.
 * @return None. Must be called by the current execution owner.
 */
void kernel_execution_unlock_irqrestore(uint64_t flags)
{
    uint32_t raw_cpu = smp_current_cpu();
    uint32_t cpu = execution_cpu_index(raw_cpu);
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] > 1) {
        --execution_depth[cpu];
        kernel_irq_restore(flags);
        return;
    }
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu && execution_depth[cpu] == 1) {
        uint64_t outer_flags = execution_outer_flags[cpu];
#ifdef CONFIG_LOCK_STAT
        lock_profile_release(raw_cpu);
#endif
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
    uint32_t raw_cpu = smp_current_cpu();
    uint32_t cpu = execution_cpu_index(raw_cpu);
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
#ifdef CONFIG_LOCK_STAT
    lock_profile_release(raw_cpu);
#endif
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
    uint32_t raw_cpu = smp_current_cpu();
    uint32_t cpu = execution_cpu_index(raw_cpu);
    if (__atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu ||
        execution_depth[cpu] != 0) {
        kernel_irq_restore(saved);
        return -35;
    }

    token->wait_exit_irq_flags = saved;
    uint64_t resume_flags;
    kernel_execution_lock_scoped_irqsave(&resume_flags, LOCK_SCOPE_RESUMED);
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
#ifdef CONFIG_LOCK_STAT
/** @brief Attempt private-memory reader admission with a fixed profiling scope.
 * @param flags Optional writable original IRQ-state output for matching unlock.
 * @param scope Fixed profiling ID; invalid values drop only the sample.
 * @return True if held, false with the caller's original IRQ state restored.
 * Task/fault context; no recursion, upgrade, scheduling or ownership transfer. */
bool kernel_execution_try_read_lock_scoped_irqsave(uint64_t *flags, uint32_t scope)
#else
/** @brief Attempt private-memory reader admission without nesting or upgrade.
 * @param flags Optional writable original IRQ-state output for matching unlock.
 * @return True if held, false with the caller's original IRQ state restored.
 * Task/fault context; no recursion, scheduling or ownership transfer. */
bool kernel_execution_try_read_lock_irqsave(uint64_t *flags)
#endif
{
    uint64_t saved = kernel_irq_save();
    if (flags) *flags = saved;
#ifdef CONFIG_LOCK_STAT
    struct lock_profile_sample sample = {0};
    if (lock_profile_enabled()) lock_profile_begin(&sample, smp_current_cpu(), scope);
#endif
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
#ifdef CONFIG_LOCK_STAT
    lock_profile_acquired(&sample, false);
#endif
    return true;
}

#ifdef CONFIG_LOCK_STAT
/** @brief Attempt a nonrecursive reader gate with generic attribution.
 * @param flags Optional writable original IRQ-state output for matching unlock.
 * @return True if held, false with the caller's IRQ state restored.
 * Task/fault context; do not nest, upgrade, schedule or transfer ownership. */
bool kernel_execution_try_read_lock_irqsave(uint64_t *flags)
{
    return kernel_execution_try_read_lock_scoped_irqsave(flags, LOCK_SCOPE_GENERIC);
}
#endif

/** @brief Release a successful reader and restore its original IRQ state.
 * @param flags IRQ state from the matching successful reader admission.
 * The current context owns the reader; no scheduling, allocation or upgrade. */
void kernel_execution_read_unlock_irqrestore(uint64_t flags)
{
#ifdef CONFIG_LOCK_STAT
    if (lock_profile_enabled()) lock_profile_release(smp_current_cpu());
#endif
    __atomic_fetch_sub(&execution_readers, 1, __ATOMIC_SEQ_CST);
    kernel_irq_restore(flags);
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(kernel_execution_lock_irqsave);
EXPORT_SYMBOL(kernel_execution_unlock_irqrestore);

EXPORT_SYMBOL(kernel_spin_init);

EXPORT_SYMBOL(kernel_spin_lock);

EXPORT_SYMBOL(kernel_spin_unlock);

EXPORT_SYMBOL(kernel_spin_lock_irqsave);
EXPORT_SYMBOL(kernel_spin_unlock_irqrestore);

EXPORT_SYMBOL(kernel_execution_resume_irqsave);
EXPORT_SYMBOL(kernel_execution_suspend_irqrestore);

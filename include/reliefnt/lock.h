/*
 * ReliefOS kernel synchronization primitives.
 * Provides interrupt-safe spin locks for short non-sleeping critical sections.
 */
#ifndef RELIEFNT_LOCK_H
#define RELIEFNT_LOCK_H

#include <reliefnt/types.h>

struct kernel_spinlock {
    volatile uint32_t state;
};

#define KERNEL_SPINLOCK_INIT { 0u }

/** @brief Initialize an exclusively owned, unpublished lock to unlocked.
 * @param lock Caller-owned lock storage; NULL is ignored. No wait/allocation;
 * must not reset an active lock or one reachable by another CPU/IRQ.
 */
void kernel_spin_init(struct kernel_spinlock *lock);
/** @brief Acquire CPU-wide exclusion while polling required TLB progress.
 * @param lock Stable shared lock, nullable no-op. Nonrecursive; caller controls
 * IRQ exclusion and lock order, never waits on an IRQ that needs this lock.
 * No scheduling/allocation or ownership transfer; release on the same path.
 */
void kernel_spin_lock(struct kernel_spinlock *lock);
/** @brief Publish prior protected accesses before relinquishing lock ownership.
 * @param lock Stable lock owned by this execution context, NULL accepted.
 * No wait/allocation/IRQ change; does not release unrelated lifetime references.
 */
void kernel_spin_unlock(struct kernel_spinlock *lock);
uint64_t kernel_irq_save(void);
void kernel_irq_restore(uint64_t flags);
/** @brief Disable local interrupts and acquire one nonrecursive spinlock.
 * @param lock Stable shared lock, nullable no-op. @param flags Optional saved IRQ output.
 * IRQ/task safe; preserves the caller's original IRQ state and never schedules
 * or allocates. Caller controls lock order, holds no recursive instance and
 * releases on the same context with kernel_spin_unlock_irqrestore.
 */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags);
/** @brief Release a spinlock before restoring the matching caller IRQ state.
 * @param lock Borrowed lock held by this context, nullable.
 * @param flags Exact IRQ state from its matching acquisition.
 * IRQ/task safe, no allocation, wait or resource-reference transfer. Restoring
 * enabled IRQs occurs only after protected accesses have been published.
 */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags);
/**
 * @brief Acquire recursive execution ownership with FIFO admission and local IRQs masked.
 * @param flags Optional writable original IRQ-state output, paired with the matching unlock.
 * @par Context
 * Boot/task context; recursion is valid only in the same execution context. Spins without scheduling, polls membarrier rendezvous, and excludes execution readers. Never hold across a scheduler wait; release every recursion level on the same context.
 */
void kernel_execution_lock_irqsave(uint64_t *flags);
/**
 * @brief Release one recursive execution ownership level.
 * @param flags IRQ state returned by the matching lock call; the outermost
 * release restores the transaction's original IRQ state, including after resume.
 * @return None. The caller must be the current owner.
 */
void kernel_execution_unlock_irqrestore(uint64_t flags);

/** Token for one outer execution transaction suspended around a waitable task phase. */
struct kernel_execution_suspend_token {
    uint64_t original_irq_flags;
    uint64_t wait_irq_flags;
    uint64_t wait_exit_irq_flags;
    uint32_t suspended_cpu;
    uint32_t active;
};

/**
 * @brief Release only an outermost execution owner while preserving its final IRQ restore state.
 * @param token Zero-initialized storage receiving the transaction, owner CPU and IRQ states.
 * @return 0 when suspended, -22 for bad input, -1 without current ownership, or -35 for
 * nested ownership. Errors restore the pre-call IRQ state and do not change ownership.
 */
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token);
/**
 * @brief Reacquire a suspended execution transaction without enabling IRQs under ownership.
 * @param token Active suspension token; successful resume consumes it.
 * @return 0 with execution ownership restored, -22 for an inactive token, or -35 if
 * the current context already has execution ownership. Failure preserves the active token.
 */
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token);

/** @brief Try a private-memory transaction without nesting or upgrade.
 * @param flags Optional writable original IRQ-state output for matching unlock.
 * @return True if held, false with no lock and original IRQ state restored.
 * Task/fault context; no scheduling or transfer of reader ownership. */
bool kernel_execution_try_read_lock_irqsave(uint64_t *flags);
/** @brief Release a successful private-memory reader and restore local IRQs.
 * @param flags IRQ state returned by matching successful reader admission.
 * The same context owns the reader; no scheduling or allocation. */
void kernel_execution_read_unlock_irqrestore(uint64_t flags);

#endif

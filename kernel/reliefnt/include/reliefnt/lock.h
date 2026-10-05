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

void kernel_spin_init(struct kernel_spinlock *lock);
void kernel_spin_lock(struct kernel_spinlock *lock);
void kernel_spin_unlock(struct kernel_spinlock *lock);
uint64_t kernel_irq_save(void);
void kernel_irq_restore(uint64_t flags);
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags);
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags);
/* Serializes non-reentrant kernel service paths while user code runs in
 * parallel on separate CPUs. Never hold it across a scheduler wait. */
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
 * @brief Release a depth-one execution transaction and enable interrupts for a waitable phase.
 * @param token Zero-initialized storage receiving the transaction and IRQ state to restore.
 * @return 0 when suspended, -EINVAL for bad input, -EPERM without current ownership, or
 * -EDEADLK for nested ownership. Failure leaves execution ownership and IRQ state unchanged.
 */
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token);
/**
 * @brief Reacquire a suspended execution transaction with IRQs masked.
 * @param token Active token returned by kernel_execution_suspend_irqrestore.
 * @return 0 with ownership restored, -EINVAL for an inactive token, or -EDEADLK if
 * the current context already owns a transaction. On success IRQs stay masked until
 * the original outer unlock restores its saved state.
 */
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token);

/* Try a private-memory transaction. No nesting, upgrades, or scheduling.
 * On failure, no lock is held and the original IRQ state is restored. */
bool kernel_execution_try_read_lock_irqsave(uint64_t *flags);
void kernel_execution_read_unlock_irqrestore(uint64_t flags);

#endif

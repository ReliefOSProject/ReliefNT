/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_LOCK_PROFILE_H
#define RELIEFNT_LOCK_PROFILE_H
#include <reliefnt/lock.h>

/* Fixed names only: neither callers nor this interface expose kernel pointers. */
enum lock_profile_scope {
    LOCK_SCOPE_GENERIC, LOCK_SCOPE_SYSCALL_IO, LOCK_SCOPE_SYSCALL_PROCESS,
    LOCK_SCOPE_SYSCALL_MEMORY, LOCK_SCOPE_SYSCALL_OTHER, LOCK_SCOPE_FAULT,
    LOCK_SCOPE_SCHED, LOCK_SCOPE_RESUMED, LOCK_SCOPE_COUNT
};
#define LOCK_PROFILE_BINS 32u

struct lock_profile_stats {
    uint64_t acquisitions, contentions, wait_total, wait_max;
    uint64_t hold_total, hold_max, irq_total, irq_max;
    uint64_t wait_hist[LOCK_PROFILE_BINS], hold_hist[LOCK_PROFILE_BINS];
    uint64_t irq_hist[LOCK_PROFILE_BINS];
    uint64_t clock_errors;
};

#ifdef CONFIG_LOCK_STAT
struct lock_profile_sample { uint64_t state, start; uint32_t cpu, scope; };

/** @brief Check whether runtime sampling is enabled without reading a clock.
 * @return True only for the currently enabled session. Atomic read only;
 * IRQ-safe, no allocation, scheduling, counter writes or ownership change. */
bool lock_profile_enabled(void);
/** @brief Start an optional sample after masking IRQs; invalid IDs are dropped.
 * @param sample Required caller-owned writable storage.
 * @param cpu Physical CPU index, invalid indices produce an inactive sample.
 * @param scope Fixed scope ID, invalid values produce an inactive sample.
 * Local IRQs must already be masked; no lock, allocation or scheduling. */
void lock_profile_begin(struct lock_profile_sample *sample, uint32_t cpu, uint32_t scope);
/** @brief Record a successful outer admission if its session is still current.
 * @param sample Required borrowed result from the matching begin call.
 * @param contended True if the acquisition spun before admission.
 * Local IRQs remain masked; CPU ownership cannot migrate and no lock is taken. */
void lock_profile_acquired(const struct lock_profile_sample *sample, bool contended);
/** @brief Finish this CPU's current outer segment before restoring interrupts.
 * @param cpu Current physical CPU; old sessions and invalid indices are ignored.
 * Local IRQs remain masked; no allocation, scheduling or ownership transfer. */
void lock_profile_release(uint32_t cpu);
/** @brief Enter the FIFO execution gate with a fixed diagnostic scope.
 * @param flags Optional writable original IRQ output, paired with unlock.
 * @param scope Fixed scope ID; invalid IDs suppress only profiling.
 * Boot/task context; spins without scheduling and permits same-context
 * recursion. Never hold across a scheduler wait; release on the same context. */
void kernel_execution_lock_scoped_irqsave(uint64_t *flags, uint32_t scope);
/** @brief Attempt reader admission and label only a successful segment.
 * @param flags Optional writable original IRQ output, paired with unlock.
 * @param scope Fixed scope ID; invalid IDs suppress only profiling.
 * @return True only when held; false restores the caller's IRQ state.
 * Task/fault context; do not nest, upgrade, schedule or transfer ownership. */
bool kernel_execution_try_read_lock_scoped_irqsave(uint64_t *flags, uint32_t scope);

/** @brief Apply one complete control value with the execution gate held.
 * @param command Required validated readable bytes, not necessarily terminated.
 * @param length Exact byte count, including an optional trailing newline.
 * @return Zero or negative EINVAL/EOVERFLOW, with state unchanged on error.
 * Controls serialize via execution ownership; no allocation or clock read. */
int lock_profile_control(const char *command, uint32_t length);
/** @brief Read one CPU/scope; fields are atomic but not globally simultaneous.
 * @param cpu Physical CPU index below SMP_MAX_CPUS.
 * @param scope Fixed scope ID below LOCK_SCOPE_COUNT.
 * @param out Required caller-owned writable counter block.
 * @return Zero or negative EINVAL. No lock, allocation or ownership transfer. */
int lock_profile_snapshot(uint32_t cpu, uint32_t scope, struct lock_profile_stats *out);
/** @brief Render a live diagnostic at an arbitrary byte offset.
 * @param offset Start position in the rendered stream.
 * @param buffer Caller-owned writable output, nullable at zero capacity.
 * @param capacity Available output bytes.
 * @param read Required writable byte count for valid input.
 * @return Zero or negative EINVAL; invalid buffer arguments leave read alone,
 * and consecutive live reads may differ.
 * Task context under execution ownership; no allocation or ownership transfer. */
int lock_profile_read(uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read);
/** @brief Capture a stable read view; allocation occurs outside hot hooks.
 * @param out Required writable output receiving an owned view or NULL on error.
 * @return Zero or negative ENOMEM/EINVAL. Task context under execution
 * ownership; caller later consumes the result with lock_profile_view_free. */
int lock_profile_view_new(void **out);
/** @brief Read a captured view at an arbitrary byte offset.
 * @param view Required borrowed live open capture.
 * @param offset Start position in the rendered stream.
 * @param buffer Caller-owned writable output, nullable at zero capacity.
 * @param capacity Available output bytes.
 * @param read Required writable byte count for valid view/input.
 * @return Zero or negative EINVAL. Task context under execution ownership;
 * no allocation, ownership transfer or mutation of the capture. */
int lock_profile_view_read(void *view, uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read);
/** @brief Consume an optional view allocated at open.
 * @param view Owned optional capture, NULL accepted.
 * Task context under execution ownership; the consumed view is no longer valid. */
void lock_profile_view_free(void *view);
/** @brief Classify a saved syscall number using fixed source-safe IDs.
 * @param number Linux syscall number from the entry frame.
 * @return Fixed scope ID, never a pointer. Pure IRQ-safe calculation without
 * allocation, mutable state or ownership transfer. */
uint32_t lock_profile_syscall_scope(uint64_t number);
#else
/* Scope arguments are deliberately not evaluated in the profile-off build. */
#define kernel_execution_lock_scoped_irqsave(flags, scope) kernel_execution_lock_irqsave(flags)
#define kernel_execution_try_read_lock_scoped_irqsave(flags, scope) kernel_execution_try_read_lock_irqsave(flags)
#endif
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_KTHREAD_H
#define RELIEFNT_KTHREAD_H
#include <reliefnt/types.h>
struct module;
struct trap_frame;
struct kthread;
typedef int (*kthread_fn)(void *);
/** @brief Create a stopped kernel thread on an independent retained stack.
 * @param function Required permanent/owner-pinned entry, returning an exit code.
 * @param context Borrowed state retained until join. @param name Task name.
 * @param owner COMING/LIVE module or NULL for permanent code.
 * @param cpu Discovered CPU index; stacks never migrate, including wait scopes.
 * @param out Required owned handle output, NULL on failure.
 * @return Zero or EINVAL/ENOMEM/registration error. Task context; first wake
 * admits LIVE module execution. Module ledger stops/joins before code release.
 */
int kthread_create(kthread_fn function,void *context,const char *name,struct module *owner,uint32_t cpu,struct kthread **out);
/** @brief Coalesce a wake without losing an event published before sleep.
 * @param thread Stable handle. @return True on admission, false after exit,
 * destruction or before its owner is LIVE. IRQ/task safe, allocation-free.
 */
bool kthread_wake(struct kthread *thread);
/** @brief Borrow the running kernel thread, never the interrupted user task.
 * @return Current thread or NULL. Task/IRQ snapshot, no lifetime transfer.
 */
struct kthread *kthread_current(void);
/** @brief Test the running thread's cooperative stop request.
 * @return True after stop; false outside a kernel thread. No side effects.
 */
bool kthread_should_stop(void);
/** @brief Save this thread's continuation and yield to the CPU dispatcher.
 * @return Zero on resume or EPERM/EWOULDBLOCK in an unsafe context.
 * Task context, IRQs enabled, no held spinlock or execution transaction.
 */
int kthread_yield(void);
/** @brief Sleep until a monotonic tick, wake event or stop request.
 * @param deadline Absolute time_ticks value, UINT64_MAX for an event-only wait.
 * @return Zero on resume or EPERM/EWOULDBLOCK. Wake-before-sleep is consumed;
 * caller checks its predicate again. Same unlocked context as kthread_yield.
 */
int kthread_sleep_until(uint64_t deadline);
/** @brief Let local threads progress during an explicit synchronous drain.
 * @return Zero or EWOULDBLOCK. IRQs enabled and no locks; a thread sleeps one
 * tick, while an external task pumps local threads and waits for an IRQ.
 * Ordinary syscall admission never invokes this explicit-wait operation.
 */
int kthread_wait_tick(void);
/** @brief Await final return without releasing caller-owned handle storage.
 * @param thread Stable handle. @param result Optional exit-code output.
 * @return Zero or EINVAL/EDEADLK/EWOULDBLOCK. Task context, no callback-needed
 * locks; a depth-one execution transaction is suspended while waiting.
 */
int kthread_join(struct kthread *thread,int *result);
/** @brief Request cooperative stop, wake a sleeper, and join its final return.
 * @param thread Stable handle. @param result Optional exit-code output.
 * @return Zero or join errno; a never-started body returns EINTR without entry.
 * Threads check kthread_should_stop; stop never abandons a live stack or lock.
 */
int kthread_stop(struct kthread *thread,int *result);
/** @brief Stop/join and consume a permanent-code handle, or disable ledger work.
 * @param thread Owned handle. @return Zero or stop errno. Module-owned storage
 * is released only by its ledger after every registered source is quiescent.
 */
int kthread_destroy(struct kthread *thread);
/** @brief Check whether this CPU has runnable retained kernel contexts.
 * @return Lock-free ready hint; no context switch. IRQ/task safe.
 */
bool kthread_runnable(void);
/** @brief Wake deadline sleepers without running their entry points.
 * IRQ-safe timer hook; no allocation, execution gate or callback entry.
 */
void kthread_tick(void);
/** @brief Dispatch bounded kernel contexts from an IRQ-disabled scheduler gate.
 * @param budget Maximum turns. @return Completed turns. Caller holds exactly
 * one execution writer; it is released before switching and restored on return.
 * User task/CR3 reservations stay retained across the independent context.
 */
uint32_t kthread_dispatch(uint32_t budget);
/** @brief Dispatch at a user-return boundary while reserving a user tick.
 * @param budget Maximum turns. @return Completed turns, zero during cooldown.
 * Same writer/IRQ contract as kthread_dispatch. After a turn completes, a
 * user window until the next tick is reserved before another dispatch, capped
 * at one scheduler-clock tick even when PIT delivery stalls. Idle and explicit
 * synchronous drain paths use kthread_dispatch directly.
 */
uint32_t kthread_dispatch_user(uint32_t budget);
/** @brief Preempt a retained thread after acknowledging a timer/reschedule IRQ.
 * @param frame Borrowed CPL0 interrupt frame on this thread's private stack.
 * @return True after a permitted switch/resume, false for any critical section
 * or foreign stack. Called with IRQs disabled, never from NMI/exception entry.
 */
bool kthread_preempt(struct trap_frame *frame);
#endif

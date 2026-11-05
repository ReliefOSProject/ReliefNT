/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEFERRED_H
#define RELIEFNT_DEFERRED_H
#include <reliefnt/types.h>
enum kernel_deferred_service { DEFERRED_USB, DEFERRED_VFS, DEFERRED_GRAPHICS, DEFERRED_AUDIO };
/** @brief Coalesce a permanent dispatcher without executing provider callbacks.
 * @param service Bounded core service selector; invalid values are ignored.
 * IRQ/task safe, allocation-free. Standalone subsystem fixtures can omit the
 * permanent service executor.
 * Production Kbuild always links it, and scheduler/start entry points below
 * provide strong references to that translation unit. */
void kernel_deferred_request(enum kernel_deferred_service service) __attribute__((weak));
/** @brief Coalesce one permanent service callback from IRQ or task context.
 * @param service Bounded core service selector. No allocation or callback.
 */
static inline void kernel_deferred_schedule(enum kernel_deferred_service service)
{ if(kernel_deferred_request)kernel_deferred_request(service); }
/** @brief Start worker contexts and protocol maintenance after init owns PID 1. */
void kernel_deferred_start(void);
/** @brief Dispatch one retained kernel turn from the scheduler service stack.
 * Caller holds depth-one execution ownership; input is processed around dispatch.
 */
void kernel_deferred_run(void);
#endif

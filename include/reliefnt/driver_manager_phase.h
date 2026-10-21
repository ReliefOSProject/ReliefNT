#ifndef RELIEFNT_DRIVER_MANAGER_PHASE_H
#define RELIEFNT_DRIVER_MANAGER_PHASE_H

#include <reliefnt/lock.h>

struct driver_manager_wait_scope {
    struct kernel_execution_suspend_token execution;
    uint32_t active;
};

/**
 * @brief Admit one driver-manager operation without waiting behind a suspended manager owner.
 * @return 0 when admitted or -EBUSY when another load/unload/rescan owns manager state.
 */
int driver_manager_phase_try_enter(void);
/**
 * @brief Release the current driver-manager admission gate.
 * @return None. The caller must have successfully entered the gate.
 */
void driver_manager_phase_leave(void);
/** @brief Process deferred audio work before admitting a syscall's I/O pin.
 * @return None. Task context with depth-one execution ownership. Nonblocking
 * manager admission protects module images; STOP/close and card task callbacks
 * run with execution ownership suspended and IRQs enabled. Busy or failed
 * admission preserves queued work; task callbacks share a total budget of 32.
 */
void driver_manager_process_audio_faults(void);
/**
 * @brief Suspend the depth-one execution transaction before a waitable driver phase.
 * @param scope Zero-initialized storage receiving the execution and IRQ wait token.
 * @return 0 when the phase may wait or a negative errno if the transaction cannot be suspended.
 */
int driver_manager_wait_begin(struct driver_manager_wait_scope *scope);
/**
 * @brief Resume the execution transaction after a waitable driver phase.
 * @param scope Active scope returned by driver_manager_wait_begin.
 * @return 0 with execution ownership restored or a negative errno on invalid ownership.
 */
int driver_manager_wait_end(struct driver_manager_wait_scope *scope);
/**
 * @brief Publish legacy v1 manager callbacks after their module initialization succeeds.
 * @param owner Loaded driver slot whose service callbacks may be entered.
 * @return None. IRQ-safe admission state; caller publishes the module under its transaction.
 */
void driver_manager_service_enable(uint32_t owner);
/**
 * @brief Close admission to legacy manager callbacks owned by one module.
 * @param owner Driver slot whose service callbacks are being retired.
 * @return None. New pins fail after this IRQ-safe state change.
 */
void driver_manager_service_close(uint32_t owner);
/**
 * @brief Reopen legacy service admission after a failed unload that retained the module.
 * @param owner Still-loaded driver slot whose callbacks remain valid.
 * @return None. No module state is released by this operation.
 */
void driver_manager_service_reopen(uint32_t owner);
/**
 * @brief Permanently disable legacy service admission for an unloaded module.
 * @param owner Driver slot being cleared after callback drain.
 * @return None. Existing callbacks must already have drained.
 */
void driver_manager_service_disable(uint32_t owner);
/**
 * @brief Pin one published legacy callback while it may execute module code.
 * @param owner Driver slot owning the callback.
 * @return True when admitted and pinned; false while absent or closing.
 */
bool driver_manager_service_try_pin(uint32_t owner);
/**
 * @brief Drop one legacy callback pin previously acquired for owner.
 * @param owner Driver slot whose callback has returned.
 * @return None. IRQ-safe reference release.
 */
void driver_manager_service_unpin(uint32_t owner);
/**
 * @brief Wait in task context until all already-admitted callbacks for owner return.
 * @param owner Driver slot whose callbacks have been closed to new admission.
 * @return None. Must run without execution ownership; does not hold the service registry lock.
 */
void driver_manager_service_drain(uint32_t owner);

#endif

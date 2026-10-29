/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/usb.h>
#include <reliefnt/driver_manager_phase.h>

/** @brief Service USB maintenance outside IRQ and the global execution owner.
 * Called at a scheduler boundary or before a syscall pins user I/O. Manager admission excludes load/unload;
 * the registry pins each LIVE callback. A busy gate preserves queued work.
 * No scheduler/registry spinlock spans port debounce, transfer or DMA teardown.
 */
void usb_process_work(void)
{
    if(!usb_service_pending() || driver_manager_phase_try_enter())return;
    struct driver_manager_wait_scope scope={0};
    if(!driver_manager_wait_begin(&scope)) {
        usb_service_run();
        (void)driver_manager_wait_end(&scope);
    }
    driver_manager_phase_leave();
}

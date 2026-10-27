/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include <reliefnt/audio.h>
#include <reliefnt/driver_manager_phase.h>

/** @brief Drain deferred audio work before a syscall admits its user I/O pin.
 * Depth-one execution-owned task context; service admission retains sound-core
 * code across explicit execution suspension. Manager admission protects hardware
 * resources while IRQs progress; busy/failed admission preserves pending work.
 * No service/module lock spans STOP, callback, wait or final PCM release.
 */
void audio_core_process_faults(void)
{
    if((!audio_disconnect_work_pending() && !audio_pcm_release_work_pending() &&
        !audio_task_work_pending()) || driver_manager_phase_try_enter())return;
    struct driver_manager_wait_scope scope={0};
    if(!driver_manager_wait_begin(&scope)) {
        (void)audio_process_pending_disconnects(16u);
        (void)audio_pcm_retry_releases(32u);
        (void)audio_process_task_work(32u);
        if(!driver_manager_wait_end(&scope))audio_pcm_finish_releases();
    }
    driver_manager_phase_leave();
}

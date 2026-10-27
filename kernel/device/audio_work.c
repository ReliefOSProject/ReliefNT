/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/sound_service.h>

/** @brief Process deferred audio work before admitting a syscall's I/O pin.
 * @return None. Task context with depth-one execution ownership. Nonblocking
 * manager admission protects module images; STOP/close and card task callbacks
 * run with execution ownership suspended and IRQs enabled. Busy or failed
 * admission preserves queued work; task callbacks share a total budget of 32.
 */
void driver_manager_process_audio_faults(void)
{
    sound_service_process_faults();
}

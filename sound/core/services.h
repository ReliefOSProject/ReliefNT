/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SOUND_CORE_SERVICES_H
#define RELIEFNT_SOUND_CORE_SERVICES_H
#include <reliefnt/types.h>
struct storage_node;
struct reliefos_dir_entry;
/** @brief Drain deferred sound work under the admitted core provider pin.
 * Depth-one execution-owned task context; manager admission protects hardware
 * while STOP, pending releases and card work temporarily suspend execution.
 * Busy/nested admission preserves pending work for a later pass. No service or
 * module lock spans callbacks, waits or final backing releases.
 */
void audio_core_process_faults(void);
/** @brief Publish the sound core's typed service callbacks during init.
 * @return Zero or registration errno. Task context, no external self-pin;
 * native COMING admission stays closed until the loader publishes LIVE.
 */
int audio_core_services_start(void);
/** @brief Detach sound callbacks after all provider calls have drained.
 * Task/exit context; idempotent after the owner ledger already detached them.
 */
void audio_core_services_stop(void);
/** @brief Resolve an ALSA control basename through the live-card registry.
 * @param name Borrowed basename. @param out Optional copied node.
 * @return Zero or ENOENT. Task context, no ownership transfer or disk access.
 */
int storage_audio_control_node(const char *name,struct storage_node *out);
/** @brief Resolve a live supported PCM basename and direction.
 * @param name Borrowed basename. @param out Optional copied node.
 * @return Zero or ENOENT. Task context; caps callbacks hold no registry lock.
 */
int storage_audio_pcm_node(const char *name,struct storage_node *out);
/** @brief Resolve the global ALSA timer name while sound is admitted.
 * @param name Borrowed basename. @param out Optional copied node.
 * @return Zero or ENOENT. Task context, no allocation or ownership transfer.
 */
int storage_audio_timer_node(const char *name,struct storage_node *out);
/** @brief Enumerate current supported ALSA control/PCM/timer names.
 * @param cursor In/out bounded namespace position. @param entry Copied output.
 * @return One entry, EOF zero or EINVAL. Task context, no retained pointers.
 */
int storage_audio_snd_readdir(uint64_t *cursor,struct reliefos_dir_entry *entry);
#endif

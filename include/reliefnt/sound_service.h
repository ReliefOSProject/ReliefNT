/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SOUND_SERVICE_H
#define RELIEFNT_SOUND_SERVICE_H
#include <reliefnt/audio.h>
#include <reliefnt/service.h>
#include <reliefnt/storage.h>

/* Internal kernel interface, not a userspace ABI. Optional methods retain their
 * documented absent-provider result. Every call pins the original LIVE table;
 * no service/module lock spans its callback, user copy, hardware wait or STOP. */
struct sound_service_operations {
    int (*card_index)(uint32_t card);
    int (*card_snapshot)(uint32_t index,uint32_t * out_id,struct audio_card_identity * identity);
    int (*register_card)(const struct audio_card_identity * identity,const struct audio_card_ops * operations,void * context,uint32_t owner,uint32_t * out_id);
    int (*unregister_card)(uint32_t card,uint32_t force);
    int (*unregister_owner)(uint32_t owner,uint32_t force);
    int (*set_service)(uint32_t card,audio_service_fn callback,void * context);
    int (*request_disconnect)(uint32_t card);
    void (*period_elapsed)(uint32_t card,uint32_t stream,uint64_t frames,int error);
    void (*control_changed)(uint32_t card,uint32_t control);
    void (*tick)(void);
    void (*process_faults)(void);
    int (*control_node)(const char * name,struct storage_node * out);
    int (*pcm_node)(const char * name,struct storage_node * out);
    int (*timer_node)(const char * name,struct storage_node * out);
    int (*snd_readdir)(uint64_t * cursor,struct reliefos_dir_entry * entry);
};
/** @brief Publish an optional sound core without an external self-reference.
 * @param provider Persistent descriptor whose operations are sound_service_operations.
 * @return Zero or registration errno. Init/task context; COMING callbacks stay
 * closed until LIVE. The owner ledger detaches and drains before code release.
 */
int sound_service_register(const struct kernel_service_descriptor *provider);
/** @brief Detach an idle exact sound provider after all admitted calls drain.
 * @param provider Exact persistent registration identity.
 * @return Zero or ENOENT/EBUSY/EINVAL. Task context, no callback under locks.
 */
int sound_service_unregister(const struct kernel_service_descriptor *provider);
/** @brief Return the compact ordinal of a live sound card.
 * @param card Generation-tagged card token.
 * @return Backend result or -ENOENT when unavailable.
 * IRQ/task-safe snapshot; unavailable cards retain no references.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_card_index(uint32_t card);
/** @brief Copy one live card identity through its retained provider.
 * @param index Zero-based live-card ordinal.
 * @param out_id Optional token output.
 * @param identity Optional copied identity output.
 * @return Backend result or -ENOENT when unavailable.
 * Task snapshot context; absent providers leave outputs untouched.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_card_snapshot(uint32_t index,uint32_t * out_id,struct audio_card_identity * identity);
/** @brief Register hardware with the optional sound core.
 * @param identity Borrowed identity copied by the sound core.
 * @param operations Borrowed hardware callbacks copied by the sound core.
 * @param context Hardware context owned until unregister completes.
 * @param owner Driver-manager slot owning the hardware.
 * @param out_id Output generation token, unchanged on failure.
 * @return Backend result or -ENODEV when unavailable.
 * Driver init/task context, preserving the original manager admission.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_register_card(const struct audio_card_identity * identity,const struct audio_card_ops * operations,void * context,uint32_t owner,uint32_t * out_id);
/** @brief Disconnect one card through the original sound provider.
 * @param card Exact registered token.
 * @param force Original disconnect/STOP policy.
 * @return Backend result or -ENODEV when unavailable.
 * Task context; caller supplies manager admission for waitable STOP.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_unregister_card(uint32_t card,uint32_t force);
/** @brief Drain sound cards before their hardware owner retires.
 * @param owner Driver-manager slot to drain.
 * @param force Original disconnect/STOP policy.
 * @return Backend result or 0 when unavailable.
 * Task/manager teardown context. An absent sound core owns no cards.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_unregister_owner(uint32_t owner,uint32_t force);
/** @brief Change a card service while retaining optional sound-core code.
 * @param card Live card token.
 * @param callback Nonblocking hardware service or NULL to remove.
 * @param context Borrowed state retained by hardware until removal.
 * @return Backend result or -ENODEV when unavailable.
 * Task context; preserves the original callback-drain protocol.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_set_service(uint32_t card,audio_service_fn callback,void * context);
/** @brief Queue a hardware disconnect through the admitted core.
 * @param card Controller card token.
 * @return Backend result or -ENODEV when unavailable.
 * IRQ/task safe; no waitable STOP is performed here.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_request_disconnect(uint32_t card);
/** @brief Report completed audio frames without exposing optional code addresses.
 * @param card Card generation token.
 * @param stream Stream identity.
 * @param frames Completed frame count.
 * @param error Hardware completion error or zero.
 * IRQ/task safe; a missing provider has no active streams to notify.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_period_elapsed(uint32_t card,uint32_t stream,uint64_t frames,int error);
/** @brief Notify control readers through the retained optional core.
 * @param card Card generation token.
 * @param control Changed control identity.
 * IRQ/task safe; an absent core owns no control readers.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_control_changed(uint32_t card,uint32_t control);
/** @brief Run the optional bounded sound IRQ service.
 * IRQ context; this wrapper neither allocates nor waits.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_tick(void);
/** @brief Run deferred sound work under a provider execution pin.
 * Depth-one execution-owned task context; the provider may temporarily suspend execution for STOP and callbacks.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_process_faults(void);
/** @brief Resolve a registered ALSA control device.
 * @param name Borrowed devfs basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Execution-owned task context; unavailable providers leave output untouched.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_control_node(const char * name,struct storage_node * out);
/** @brief Resolve a supported ALSA PCM device and direction.
 * @param name Borrowed Linux PCM basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Task context; provider caps callbacks run outside core registry locks.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_pcm_node(const char * name,struct storage_node * out);
/** @brief Resolve the optional core global ALSA timer.
 * @param name Borrowed timer basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Execution-owned task context; no timer node is exposed while absent.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_timer_node(const char * name,struct storage_node * out);
/** @brief Enumerate only the admitted sound namespace.
 * @param cursor In/out namespace position.
 * @param entry Copied directory entry output.
 * @return Backend result or 0 when unavailable.
 * Task context; absence returns EOF without advancing the cursor.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_snd_readdir(uint64_t * cursor,struct reliefos_dir_entry * entry);
#endif

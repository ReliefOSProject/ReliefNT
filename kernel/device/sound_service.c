/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/sound_service.h>
#include <reliefnt/export.h>
#include <linux/errno.h>
static struct kernel_service_slot sound_service_slot;
/** @brief Attach the exact immutable sound table to its permanent core slot.
 * @param provider Persistent sound_service_operations descriptor and owner.
 * @return Zero or generic registration errno. Init/task context; no lock spans
 * ledger allocation, and no self user-reference prevents normal core unload.
 */
int sound_service_register(const struct kernel_service_descriptor *provider)
{return kernel_service_register(&sound_service_slot,provider);}
/** @brief Close an idle sound table without invoking optional code.
 * @param provider Exact persistent identity.
 * @return Zero or generic ENOENT/EBUSY/EINVAL. Task context, no callback/I/O.
 */
int sound_service_unregister(const struct kernel_service_descriptor *provider)
{return kernel_service_unregister(&sound_service_slot,provider);}
/** @brief Return the compact ordinal of a live sound card.
 * @param card Generation-tagged card token.
 * @return Backend result or -ENOENT when unavailable.
 * IRQ/task-safe snapshot; unavailable cards retain no references.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_card_index(uint32_t card)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENOENT;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->card_index ? provider_ops->card_index(card) : -ENOENT;
    kernel_service_release(&call);return result;
}
/** @brief Copy one live card identity through its retained provider.
 * @param index Zero-based live-card ordinal.
 * @param out_id Optional token output.
 * @param identity Optional copied identity output.
 * @return Backend result or -ENOENT when unavailable.
 * Task snapshot context; absent providers leave outputs untouched.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_card_snapshot(uint32_t index,uint32_t * out_id,struct audio_card_identity * identity)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENOENT;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->card_snapshot ? provider_ops->card_snapshot(index,out_id,identity) : -ENOENT;
    kernel_service_release(&call);return result;
}
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
int sound_service_register_card(const struct audio_card_identity * identity,const struct audio_card_ops * operations,void * context,uint32_t owner,uint32_t * out_id)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENODEV;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->register_card ? provider_ops->register_card(identity,operations,context,owner,out_id) : -ENODEV;
    kernel_service_release(&call);return result;
}
/** @brief Disconnect one card through the original sound provider.
 * @param card Exact registered token.
 * @param force Original disconnect/STOP policy.
 * @return Backend result or -ENODEV when unavailable.
 * Task context; caller supplies manager admission for waitable STOP.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_unregister_card(uint32_t card,uint32_t force)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENODEV;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->unregister_card ? provider_ops->unregister_card(card,force) : -ENODEV;
    kernel_service_release(&call);return result;
}
/** @brief Drain sound cards before their hardware owner retires.
 * @param owner Driver-manager slot to drain.
 * @param force Original disconnect/STOP policy.
 * @return Backend result or 0 when unavailable.
 * Task/manager teardown context. An absent sound core owns no cards.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_unregister_owner(uint32_t owner,uint32_t force)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return 0;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->unregister_owner ? provider_ops->unregister_owner(owner,force) : 0;
    kernel_service_release(&call);return result;
}
/** @brief Change a card service while retaining optional sound-core code.
 * @param card Live card token.
 * @param callback Nonblocking hardware service or NULL to remove.
 * @param context Borrowed state retained by hardware until removal.
 * @return Backend result or -ENODEV when unavailable.
 * Task context; preserves the original callback-drain protocol.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_set_service(uint32_t card,audio_service_fn callback,void * context)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENODEV;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->set_service ? provider_ops->set_service(card,callback,context) : -ENODEV;
    kernel_service_release(&call);return result;
}
/** @brief Queue a hardware disconnect through the admitted core.
 * @param card Controller card token.
 * @return Backend result or -ENODEV when unavailable.
 * IRQ/task safe; no waitable STOP is performed here.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_request_disconnect(uint32_t card)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENODEV;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->request_disconnect ? provider_ops->request_disconnect(card) : -ENODEV;
    kernel_service_release(&call);return result;
}
/** @brief Report completed audio frames without exposing optional code addresses.
 * @param card Card generation token.
 * @param stream Stream identity.
 * @param frames Completed frame count.
 * @param error Hardware completion error or zero.
 * IRQ/task safe; a missing provider has no active streams to notify.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_period_elapsed(uint32_t card,uint32_t stream,uint64_t frames,int error)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return;
    const struct sound_service_operations *provider_ops=call.operations;
    if(provider_ops->period_elapsed)provider_ops->period_elapsed(card,stream,frames,error);
    kernel_service_release(&call);
}
/** @brief Notify control readers through the retained optional core.
 * @param card Card generation token.
 * @param control Changed control identity.
 * IRQ/task safe; an absent core owns no control readers.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_control_changed(uint32_t card,uint32_t control)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return;
    const struct sound_service_operations *provider_ops=call.operations;
    if(provider_ops->control_changed)provider_ops->control_changed(card,control);
    kernel_service_release(&call);
}
/** @brief Run the optional bounded sound IRQ service.
 * IRQ context; this wrapper neither allocates nor waits.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_tick(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return;
    const struct sound_service_operations *provider_ops=call.operations;
    if(provider_ops->tick)provider_ops->tick();
    kernel_service_release(&call);
}
/** @brief Run deferred sound work under a provider execution pin.
 * Depth-one execution-owned task context; the provider may temporarily suspend execution for STOP and callbacks.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
void sound_service_process_faults(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return;
    const struct sound_service_operations *provider_ops=call.operations;
    if(provider_ops->process_faults)provider_ops->process_faults();
    kernel_service_release(&call);
}
/** @brief Resolve a registered ALSA control device.
 * @param name Borrowed devfs basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Execution-owned task context; unavailable providers leave output untouched.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_control_node(const char * name,struct storage_node * out)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENOENT;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->control_node ? provider_ops->control_node(name,out) : -ENOENT;
    kernel_service_release(&call);return result;
}
/** @brief Resolve a supported ALSA PCM device and direction.
 * @param name Borrowed Linux PCM basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Task context; provider caps callbacks run outside core registry locks.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_pcm_node(const char * name,struct storage_node * out)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENOENT;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->pcm_node ? provider_ops->pcm_node(name,out) : -ENOENT;
    kernel_service_release(&call);return result;
}
/** @brief Resolve the optional core global ALSA timer.
 * @param name Borrowed timer basename.
 * @param out Optional copied node output.
 * @return Backend result or -ENOENT when unavailable.
 * Execution-owned task context; no timer node is exposed while absent.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_timer_node(const char * name,struct storage_node * out)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return -ENOENT;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->timer_node ? provider_ops->timer_node(name,out) : -ENOENT;
    kernel_service_release(&call);return result;
}
/** @brief Enumerate only the admitted sound namespace.
 * @param cursor In/out namespace position.
 * @param entry Copied directory entry output.
 * @return Backend result or 0 when unavailable.
 * Task context; absence returns EOF without advancing the cursor.
 * No service/module lock spans callbacks; no provider reference escapes.
 */
int sound_service_snd_readdir(uint64_t * cursor,struct reliefos_dir_entry * entry)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&sound_service_slot,&call))return 0;
    const struct sound_service_operations *provider_ops=call.operations;
    int result=provider_ops->snd_readdir ? provider_ops->snd_readdir(cursor,entry) : 0;
    kernel_service_release(&call);return result;
}
EXPORT_SYMBOL(sound_service_register);
EXPORT_SYMBOL(sound_service_unregister);

/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include <reliefnt/sound_service.h>
#include <reliefnt/driver_module.h>
#include <reliefnt/export.h>
#include <linux/errno.h>

static const struct sound_service_operations audio_core_service_operations={
    .card_index=audio_card_index,.card_snapshot=audio_card_snapshot,
    .register_card=audio_register_card_owned,.unregister_card=audio_unregister_card,
    .unregister_owner=audio_unregister_owner,.set_service=audio_card_set_service,
    .request_disconnect=audio_request_controller_disconnect,.period_elapsed=audio_period_elapsed,
    .control_changed=audio_control_changed,.tick=audio_service_tick,.process_faults=audio_core_process_faults,
    .control_node=storage_audio_control_node,.pcm_node=storage_audio_pcm_node,
    .timer_node=storage_audio_timer_node,.snd_readdir=storage_audio_snd_readdir};
static const struct kernel_service_descriptor audio_core_service_provider={
    .operations=&audio_core_service_operations,.owner=THIS_MODULE};
/** @brief Publish actual sound metadata, hardware notifications and deferred work.
 * @return Zero or registration errno. Init/task context; the cleanup ledger
 * closes admission and drains execution pins before provider code is released.
 */
int audio_core_services_start(void)
{return sound_service_register(&audio_core_service_provider);}
/** @brief Remove the exact idle sound table after native/builtin teardown.
 * Task/exit context without provider callbacks; the ledger may have detached
 * it already. Pending hardware/backing leases must drain before code unload.
 */
void audio_core_services_stop(void)
{(void)sound_service_unregister(&audio_core_service_provider);}
/** @brief Register hardware through its real sound-core dependency and owner gate.
 * @param descriptor Persistent AUDIO descriptor owned by the hardware image.
 * @param owner COMING native hardware module or NULL for linked builtin code.
 * @return Zero, including no matching hardware, or manager/validation errno.
 * Init task under the loader/boot execution transaction; waitable probe/STOP
 * follows manager suspension, no registry lock spans hardware. The caller's
 * native ELF import keeps this core mapped through its eventual cleanup.
 */
int audio_driver_module_register(const struct reliefos_driver_module *descriptor,struct module *owner)
{
    if(!descriptor || descriptor->kind!=RELIEFOS_DRIVER_KIND_AUDIO)return -EINVAL;
    return driver_module_register(descriptor,owner);
}
EXPORT_SYMBOL(audio_driver_module_register);

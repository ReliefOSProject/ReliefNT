/* SPDX-License-Identifier: Apache-2.0 */
/* Real work/manager/service implementations with only the hardware table
 * boundary reduced to the work method exercised by these PCM/HDA fixtures. */
#include "../../../kernel/device/service.c"
#include "../../../kernel/device/sound_service.c"
#include "../../../kernel/device/audio_work.c"
#include "../../../sound/core/work.c"
static const struct sound_service_operations audio_fixture_work_operations={
    .process_faults=audio_core_process_faults};
static const struct kernel_service_descriptor audio_fixture_work_provider={
    .operations=&audio_fixture_work_operations};
static void audio_fixture_work_start(void)
{assert(!sound_service_register(&audio_fixture_work_provider));}
static void audio_fixture_work_stop(void)
{assert(!sound_service_unregister(&audio_fixture_work_provider));}

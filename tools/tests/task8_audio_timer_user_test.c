/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio timer user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_CONTROL_ENTRY inherited_audio_control_user_main
#define audio_timer_period_elapsed inherited_audio_timer_period_elapsed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wformat"
#include "task8_audio_control_user_test.c"
#pragma clang diagnostic pop
#undef audio_timer_period_elapsed
#undef main
#include "../../sound/core/timer.c"

int main(void)
{
    struct task task = {.pid = 721, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    struct task_file file = {.node.flags = STORAGE_NODE_FLAG_AUDIO_TIMER};
    assert(!audio_timer_open(&task, &file));
    struct audio_timer_file *timer = file.audio_timer_file;
    timer->selected = 1; timer->running = 1; timer->card = 123;
    timer->device = 2; timer->direction = AUDIO_PLAYBACK;
    audio_timer_period_elapsed(123, 2, AUDIO_PLAYBACK, 1, 0);
    assert(timer->count == 1);
    struct snd_timer_read output = {0};
    blocked = (uintptr_t)&output; blocked_output = true;
    assert(audio_timer_read(&task, &file, &output, sizeof(output)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(timer->count == 1);
    blocked = 0;
    assert(audio_timer_read(&task, &file, &output, sizeof(output)) == (int)sizeof(output));
    assert(!timer->count && output.ticks == 1);
    int version = 0;
    blocked = (uintptr_t)&version;
    assert(audio_timer_ioctl(&task, &file, SNDRV_TIMER_IOCTL_PVERSION,
                             (uintptr_t)&version) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!version);
    blocked = 0;
    assert(!audio_timer_ioctl(&task, &file, SNDRV_TIMER_IOCTL_PVERSION, (uintptr_t)&version));
    assert(version == SNDRV_TIMER_VERSION);
    assert(!audio_timer_close(&file));
    puts("PASS ALSA timer: event retained across prerequisite wait and fixed output prepared");
}

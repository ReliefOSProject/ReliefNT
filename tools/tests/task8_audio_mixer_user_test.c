/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio mixer user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_CONTROL_ENTRY inherited_audio_control_user_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wformat"
#include "task8_audio_control_user_test.c"
#pragma clang diagnostic pop
#include "../../sound/core/mixer.c"

int main(void)
{
    struct audio_test_control_card card; init_card(&card, 3);
    struct task task = {.pid = 722, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    struct task_file file = {0};
    assert(!audio_mixer_open_card(card.id, &file.audio_mixer_file));
    int volume = 0;
    blocked = (uintptr_t)&volume; blocked_output = true;
    assert(audio_mixer_ioctl(&task, &file, SOUND_MIXER_READ_VOLUME,
                             (uintptr_t)&volume) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!volume);
    blocked = 0;
    assert(!audio_mixer_ioctl(&task, &file, SOUND_MIXER_READ_VOLUME, (uintptr_t)&volume));
    assert(!audio_mixer_close_file(file.audio_mixer_file));
    assert(!audio_unregister_card(card.id, 0));
    puts("PASS ALSA-backed OSS mixer: fixed output waits before hardware access");
    return 0;
}

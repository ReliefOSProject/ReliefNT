/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio control user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_audio_control_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wformat"
#include "audio_control_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static uintptr_t blocked;
static bool blocked_output;
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{
    assert(task && task->kind == TASK_KIND_USER && length <= 32768);
    return address == blocked && (!blocked_output || output) ? SWAP_STORAGE_CONTENDED : 0;
}
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *pointer) { free(pointer); }
int task_lookup_user_bytes(struct task *task, uint64_t address, uint32_t length,
                           struct task_user_text *out)
{
    int rc = swap_prepare_user_range(task, address, length, false);
    if (rc) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (!user_range_ok(address, length)) return -EFAULT;
    char *copy = malloc(length); assert(copy);
    memcpy(copy, (const void *)(uintptr_t)address, length);
    *out = (struct task_user_text){.text = copy, .allocation = copy};
    return 0;
}
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction, uint64_t frames, int error)
{ (void)card; (void)device; (void)direction; (void)frames; (void)error; }

#ifndef TASK8_CONTROL_ENTRY
#define TASK8_CONTROL_ENTRY main
#endif
int TASK8_CONTROL_ENTRY(void)
{
    struct audio_test_control_card card; init_card(&card, 3);
    struct task task = {.pid = 720, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    struct task *entries[] = {&task}; tasks = entries; task_count = 1; current_pid[0] = task.pid;
    struct task_file file = {.node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL,
                             .node.volume_id = card.id};
    assert(!audio_control_open(&task, &file));
    int subscribe = 1;
    assert(!audio_alsa_control_ioctl(file.audio_control_file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe));
    audio_control_changed(card.id, 0);
    struct snd_ctl_event event = {0};
    blocked = (uintptr_t)&event; blocked_output = true;
    assert(audio_control_read_task(&task, &file, &event, sizeof(event)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(audio_control_poll_file(file.audio_control_file, POLLIN) & POLLIN);
    blocked = 0;
    assert(audio_control_read_task(&task, &file, &event, sizeof(event)) == (int)sizeof(event));
    assert(event.data.elem.id.numid == 1);
    int version = 0;
    blocked = (uintptr_t)&version; blocked_output = true;
    assert(audio_control_ioctl(&task, &file, SNDRV_CTL_IOCTL_PVERSION,
                               (uintptr_t)&version) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!version);
    blocked = 0;
    assert(!audio_control_ioctl(&task, &file, SNDRV_CTL_IOCTL_PVERSION, (uintptr_t)&version));
    assert(version == SNDRV_CTL_VERSION);
    assert(!audio_control_close(&file)); assert(!audio_unregister_card(card.id, 0));
    puts("PASS ALSA control: queued event and fixed output survive prerequisite contention");
    return 0;
}

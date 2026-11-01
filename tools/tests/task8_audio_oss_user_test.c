/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio oss user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_audio_oss_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "audio_oss_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static uintptr_t blocked;
static unsigned preparations;
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{
    assert(task && task->kind == TASK_KIND_USER && length <= 32768);
    (void)output;
    ++preparations;
    return address == blocked ? SWAP_STORAGE_CONTENDED : 0;
}

int main(void)
{
    struct audio_test_card card; audio_test_card_init(&card);
    struct task task = {.pid = 723, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    struct task_file file = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_WRONLY};
    assert(!audio_oss_open(&task, &file));
    uint8_t data[8192] = {0};
    blocked = (uintptr_t)data;
    assert(audio_oss_write(&task, &file, data, sizeof(data)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!file.audio_oss_file->written_bytes);
    blocked = (uintptr_t)data + 4096;
    assert(audio_oss_write(&task, &file, data, sizeof(data)) == 4096);
    assert(file.audio_oss_file->written_bytes == 4096);
    blocked = 0;
    int delay = 0; blocked = (uintptr_t)&delay;
    assert(audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETODELAY, (uintptr_t)&delay) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    blocked = 0;
    assert(!audio_oss_ioctl(&task, &file, SNDCTL_DSP_GETODELAY, (uintptr_t)&delay));
    assert(delay == 4096);
    /* SYNC's internally generated silence belongs to the kernel. */
    assert(audio_oss_write(&task, &file, data, 1) == 1);
    unsigned before = preparations;
    int result = audio_oss_ioctl(&task, &file, SNDCTL_DSP_SYNC, 0);
    assert(result == -EAGAIN || result == KERNEL_SYSCALL_BLOCKED || result == 0);
    assert(preparations == before);
    assert(!audio_oss_close(&file));
    struct task_file capture = {.flags = RELIEFOS_O_NONBLOCK | RELIEFOS_O_RDONLY};
    assert(!audio_oss_open(&task, &capture));
    assert(audio_oss_read(&task, &capture, data, 4) == -EAGAIN);
    uint8_t expected[4] = {1, 2, 3, 4};
    memcpy(card.prepared_dma.kernel, expected, 4); audio_test_advance(&card, 1, 0);
    blocked = (uintptr_t)data;
    assert(audio_oss_read(&task, &capture, data, 4) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(capture.audio_oss_file->capture_tail_bytes == 4);
    blocked = 0;
    assert(audio_oss_read(&task, &capture, data, 4) == 4 && !memcmp(data, expected, 4));
    assert(!audio_oss_close(&capture)); assert(!audio_unregister_card(card.id, 0));
    puts("PASS OSS actual core: bounded prefix, owned capture tail, fixed output, kernel silence");
    return 0;
}

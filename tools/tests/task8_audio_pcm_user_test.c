/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio pcm user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_audio_device_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpointer-bool-conversion"
#include "audio_device_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static uintptr_t blocked;
#ifdef TASK8_PCM_READY_TEST
static uintptr_t unmapped_tail;
static void (*prepare_hook)(void);
#endif
static unsigned prepares;
static bool block_output;
static uint8_t snapshot[1024];
static uint64_t snapshot_address;
static uint32_t snapshot_length;
uint32_t smp_cpu_count(void) { return 1; }
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length,
                            bool writing)
{
    assert(task && task->kind == TASK_KIND_USER && length <= 32768);
    ++prepares;
#ifdef TASK8_PCM_READY_TEST
    if(prepare_hook){void (*hook)(void)=prepare_hook;prepare_hook=NULL;hook();}
    if(unmapped_tail && address<=unmapped_tail && unmapped_tail-address<length)return -LINUX_EFAULT;
#endif
    return blocked && address <= blocked && blocked - address < length &&
        (!block_output || writing) ? SWAP_STORAGE_CONTENDED : 0;
}
int task_lookup_user_bytes(struct task *task, uint64_t address, uint32_t length,
                           struct task_user_text *out)
{
    if (!snapshot_length) {
        int rc = swap_prepare_user_range(task, address, length, false);
        if (rc) return KERNEL_SYSCALL_SWAP_CONTENDED;
        assert(length <= sizeof(snapshot) && user_range_ok(address, length));
        __builtin_memcpy(snapshot, (const void *)(uintptr_t)address, length);
        snapshot_address = address; snapshot_length = length;
    }
    assert(snapshot_address == address && snapshot_length == length);
    *out = (struct task_user_text){.text = (const char *)snapshot};
    return 0;
}

#ifdef TASK8_PCM_READY_TEST
int inherited_task8_audio_pcm_main(void)
#else
int main(void)
#endif
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task task = {.pid = 710, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    struct task *entries[] = {&task};
    tasks = entries; task_count = 1; current_pid[0] = task.pid;
    struct task_file file = audio_test_file(card.id, 0, AUDIO_PLAYBACK,
                                             RELIEFOS_O_WRONLY | RELIEFOS_O_NONBLOCK);
    assert(!audio_device_open(&task, &file));
    audio_test_configure_file(&file);
    uint8_t data[8192] = {0};
    struct audio_pcm_status before, after;
    assert(!audio_pcm_status(file.audio_file->pcm, &before));
    blocked = (uintptr_t)data;
    assert(audio_device_write(&task, &file, data, sizeof(data)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!audio_pcm_status(file.audio_file->pcm, &after));
    assert(after.appl_ptr == before.appl_ptr);
    blocked = (uintptr_t)data + 4096;
    assert(audio_device_write(&task, &file, data, sizeof(data)) == 4096);
    assert(!audio_pcm_status(file.audio_file->pcm, &after));
    assert(after.appl_ptr == before.appl_ptr + 1024);
    blocked = 0;
    assert(audio_device_write(&task, &file, data + 4096, 4096) == 4096);
    assert(prepares >= 4);
    audio_test_advance(&card, 512, 0);
    struct snd_xferi x = {.frames = 1, .buf = data, .result = -1};
    blocked = (uintptr_t)data;
    assert(audio_device_ioctl(&task, &file, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                              (uintptr_t)&x) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(x.result == -1 && snapshot_length == sizeof(x));
    x.frames = 2; x.buf = data + 4;
    blocked = 0;
    assert(!audio_device_ioctl(&task, &file, SNDRV_PCM_IOCTL_WRITEI_FRAMES,
                               (uintptr_t)&x));
    assert(x.frames == 1 && x.buf == data && x.result == 1);
    assert(!audio_device_close(&file));
    assert(!audio_unregister_card(card.id, 0));
    puts("PASS PCM actual core: prerequisite retry, no side effect, bounded accepted prefix");
    return 0;
}

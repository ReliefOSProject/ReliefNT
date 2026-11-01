/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: audio scalar front.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_scalar_user_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "task8_scalar_user_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/chrdev.h>
static unsigned callbacks;
uint32_t task_device_write_length(const struct task_file *f, uint64_t count)
{ assert(f == &file); return count > 32768 ? 32768 : (uint32_t)count; }
int64_t character_file_write(struct character_file *handle, struct task *task,
                             struct task_file *f, const void *buffer, uint64_t length)
{
    assert(handle == file.character_file && task == &current && f == &file && length <= 32768);
    ++callbacks;
    int ret = swap_prepare_user_range(task, (uintptr_t)buffer, length, false);
    return ret == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : (int64_t)length;
}
int64_t character_file_read(struct character_file *handle, struct task *task,
                            struct task_file *f, void *buffer, uint64_t length)
{
    assert(handle == file.character_file && task == &current && f == &file && length <= 4096);
    ++callbacks;
    int ret = swap_prepare_user_range(task, (uintptr_t)buffer, length, true);
    return ret == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : (int64_t)length;
}
int main(void)
{
    current.kind = TASK_KIND_USER; current.syscall_file = &file;
    file.character_file = (struct character_file *)1;
    file.audio_file = (struct audio_file *)1;
    swapped = (uintptr_t)bytes; contended = true;
    assert(syscall_io_write(3, (uintptr_t)bytes, sizeof(bytes)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(callbacks == 1);
    contended = false;
    assert(syscall_io_write(3, (uintptr_t)bytes, sizeof(bytes)) == 32768);
    swapped = (uintptr_t)bytes; contended = true;
    assert(syscall_io_read(3, (uintptr_t)bytes, sizeof(bytes)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    contended = false;
    assert(syscall_io_read(3, (uintptr_t)bytes, sizeof(bytes)) == 4096);
    unsigned before = callbacks;
    assert(syscall_io_write(3, RELIEFNT_USER_TOP - 1, 2) == -EFAULT);
    assert(callbacks == before);
    puts("PASS scalar audio admission: full numeric bounds and bounded provider preparation");
    return 0;
}

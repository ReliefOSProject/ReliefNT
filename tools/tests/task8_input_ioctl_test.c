/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: input ioctl.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_evdev_main
#include "evdev_led_test.c"
#undef main
#include <reliefnt/swap.h>

static struct task current;
static uint64_t swapped;
static bool contended;
static unsigned preparations;

struct task *sched_current_task(void) { return &current; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task == &current && length <= 32768); (void)writing; ++preparations;
    if (address == swapped && contended) return SWAP_STORAGE_CONTENDED;
    return 0;
}

int main(void)
{
    struct task_file file = {.flags = TASK_FILE_FLAG_DEV_NODE};
    file.node.first_cluster = STORAGE_DEV_KIND_KEYBOARD;
    current.kind = TASK_KIND_USER; current.pid = 1;
    input_init();
    uint8_t buffer[64] = {0};
    swapped = (uintptr_t)buffer; contended = true;
    assert(task_evdev_ioctl(&file, EVIOCGLED(16), (uintptr_t)buffer) == KERNEL_SYSCALL_SWAP_CONTENDED);
    contended = false;
    assert(task_evdev_ioctl(&file, EVIOCGLED(16), (uintptr_t)buffer) == 8);
    unsigned before = preparations;
    assert(!task_evdev_ioctl(&file, _IOC(_IOC_WRITE, 'E', 0x03, 8), 1));
    assert(preparations == before); /* Ignored operand is never dereferenced. */
    output_writable = false;
    assert(task_evdev_ioctl(&file, EVIOCGVERSION, (uintptr_t)buffer) == -RELIEFOS_EFAULT);
    output_writable = true;
    contended = true;
    assert(task_evdev_ioctl(&file, EVIOCGNAME(sizeof(buffer)), (uintptr_t)buffer) == KERNEL_SYSCALL_SWAP_CONTENDED);
    contended = false;
    assert(!task_evdev_ioctl(&file, EVIOCGNAME(sizeof(buffer)), (uintptr_t)buffer));
    assert(strstr((char *)buffer, "Keyboard"));
    puts("PASS evdev: bounded swapped output, writable/COW checks and ignored operands");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: transfer signal.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_TRANSFER_SIGNAL 1
#define main inherited_signal_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "signal_address_space_test.c"
#pragma clang diagnostic pop
#undef main

void storage_release_task_io(uint32_t pid) { assert(pid == target.pid); }

int main(void)
{
    unsigned char *old = mmap((void *)STACK_ADDRESS, sizeof(target_pages),
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(old == (void *)STACK_ADDRESS);
    target = (struct task){.kind = TASK_KIND_USER, .pid = 1, .state = TASK_READY,
                           .restart_syscall = __NR_copy_file_range + 1};
    target.limits.sigpending.rlim_cur = 8;
    target.signal_actions[2] = (struct kernel_signal_action){
        .handler = 0x410000, .restorer = 0x420000, .flags = LINUX_SA_SIGINFO};
    struct task_transfer_state pending = {.total = 3, .input_position = 4,
        .output_position = 5, .input_user = STACK_ADDRESS,
        .output_user = STACK_ADDRESS + 8, .waiting_io = true};
    target.transfer_io = &pending;
    assert(!kernel_signal_queue_task(&target, 2));
    struct trap_frame frame = {.rsp = STACK_ADDRESS + 4896, .rip = 0x430000, .cs = 0x23};
    struct trap_frame original = frame;
    signal_transfer_blocked = STACK_ADDRESS + 8;
    assert(kernel_signal_deliver_pending_outer(&target, &frame) == SWAP_STORAGE_CONTENDED);
    assert(target.pending_signals == (1ULL << 1) && target.transfer_io == &pending);
    assert(!memcmp(&frame, &original, sizeof(frame)) && drained_io == 1);
    int64_t offsets[2] = {0}; memcpy(offsets, target_pages, sizeof(offsets));
    assert(!offsets[0] && !offsets[1]);
    signal_transfer_blocked = 0;
    assert(kernel_signal_deliver_pending_outer(&target, &frame) == 1);
    struct linux_rt_sigframe saved;
    memcpy(&saved, target_pages + (frame.rsp - STACK_ADDRESS), sizeof(saved));
    memcpy(offsets, target_pages, sizeof(offsets));
    assert(saved.uc.context.rax == 3 && saved.uc.context.rip == 0x430002);
    assert(offsets[0] == 4 && offsets[1] == 5 && !target.transfer_io && drained_io == 1);
    munmap(old, sizeof(target_pages));
    puts("PASS real signal+transfer: pending request drained, offset contention keeps signal and progress");
    return 0;
}

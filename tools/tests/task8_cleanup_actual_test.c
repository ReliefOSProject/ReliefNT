/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: cleanup actual.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
/* Real descriptor cancellation + lookup scope + metadata/socket stage code. */
#define TASK8_SOCKET_NO_MAIN
#define TASK8_CLEANUP_TEST
#include "task8_socket_send_test.c"
#include "../../kernel/syscall/fs/metadata.c"

void task_regular_io_stage_cancel(struct task *task) { assert(task == &current); }
void task_vector_cancel(struct task *task) { assert(task == &current && !task->vector_io); }
void task_transfer_cancel(struct task *task) { assert(task == &current && !task->transfer_io); }
void graphics_service_cancel_task_continuation(struct task *task) { assert(task == &current); }
void task_sysv_msg_cancel(struct task *task) { assert(task == &current && !task->sysv_msg.queue); }
void task_sysv_sem_cancel(struct task *task) { assert(task == &current && !task->sysv_sem.ops); }

int main(void)
{
    current.kind = TASK_KIND_USER; current.pid = current.tgid = 10;
    current.uid = current.gid = 100; current.state = TASK_READY;
    for (unsigned i = 0; i < 2; ++i) {
        peers[i] = (struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_DGRAM,
            .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,
            .send_timeout=UINT64_MAX};
        endpoints[i] = (struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};
    }
    passed = (struct task_file){.used=1,.references=1};
    char output[32] = {0};
    struct trap_frame metadata_frame = {.rax=LINUX_SYS_FSTAT,.rdi=3,.rsi=(uintptr_t)output};
    current.syscall_file = task_file_get(&endpoints[0]);
    task_lookup_scope_begin(&current, &metadata_frame);
    struct task_lookup_path *slot = NULL;
    assert(!metadata_result_slot(&current, METADATA_RESULT_READLINK, &slot) && slot);
    char *receipt = kernel_malloc(12); assert(receipt); memcpy(receipt, "destination", 12);
    blocked = (uintptr_t)output; contended = true;
    assert(metadata_prepare_result(&current, (uintptr_t)output, 11, slot,
           METADATA_RESULT_READLINK, receipt, 12, &receipt) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!receipt && slot->name && allocations && endpoints[0].references == 2);
    task_release_syscall_file(&current);
    assert(!allocations && !current.lookup_scope.armed && !current.syscall_file && endpoints[0].references == 1);
    task_release_syscall_file(&current);
    assert(!allocations && endpoints[0].references == 1);

    struct iovec vector = {packet, sizeof(packet)};
    unsigned char control[CMSG_SPACE(sizeof(int))] __attribute__((aligned(8))) = {0};
    struct cmsghdr *cmsg = (void *)control;
    *cmsg = (struct cmsghdr){CMSG_LEN(sizeof(int)), SOL_SOCKET, SCM_RIGHTS};
    int fd = 5; memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    struct msghdr message = {.msg_iov=&vector,.msg_iovlen=1,
                             .msg_control=control,.msg_controllen=sizeof(control)};
    struct trap_frame send_frame = {.rax=LINUX_SYS_SENDMSG,.rdi=3,.rsi=(uintptr_t)&message};
    struct socket_message_result result = {0};
    current.syscall_file = task_file_get(&endpoints[0]);
    task_lookup_scope_begin(&current, &send_frame);
    blocked = (uintptr_t)packet + 32768; contended = true;
    assert(task_socket_message(&current, &endpoints[0], (uintptr_t)&message, 0, false, false,
                               &result) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(owned_stage && allocations && passed.references == 2 && !passed.scm_references);
    assert(endpoints[0].references == 2 && !peers[1].packet_count);
    task_release_syscall_file(&current);
    assert(!owned_stage && !allocations && !current.lookup_scope.armed && !current.syscall_file);
    assert(passed.references == 1 && !passed.scm_references && endpoints[0].references == 1);
    task_release_syscall_file(&current);
    assert(!allocations && passed.references == 1 && endpoints[0].references == 1);
    puts("PASS actual descriptor cleanup: metadata receipt, partial binary import and SCM stage released once");
    return 0;
}

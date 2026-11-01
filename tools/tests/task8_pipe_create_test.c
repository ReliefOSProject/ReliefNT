/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: pipe create.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_pipe_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "pipe_capacity_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
static bool contended;static unsigned prepares;
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{assert(task==&current && address && length==8 && writing);++prepares;
 return contended ? SWAP_STORAGE_CONTENDED : 0;}
struct task_file *task_file_for_fd(struct task *task,int fd)
{assert(task==&current);return fd>=0 && fd<640 && descriptors[fd].used ? &descriptors[fd] : NULL;}
int main(void)
{
    kernel_objects_init();current.uid=1000;current.kind=TASK_KIND_USER;
    int pair[2]={-1,-1};contended=true;
    assert(syscall_ipc_pipe2((uintptr_t)pair,0x80000000u)==-RELIEFOS_EINVAL && !prepares);
    assert(syscall_ipc_pipe2((uintptr_t)pair,RELIEFOS_O_NONBLOCK|RELIEFOS_O_CLOEXEC)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(pair[0]==-1 && pair[1]==-1 && !descriptors[0].used);
    contended=false;assert(!syscall_ipc_pipe2((uintptr_t)pair,RELIEFOS_O_NONBLOCK|RELIEFOS_O_CLOEXEC));
    assert(pair[0]==0 && pair[1]==1 && (descriptors[0].flags&RELIEFOS_O_NONBLOCK) &&
           (descriptors[1].fd_flags&RELIEFOS_FD_CLOEXEC));
    task_discard_file_fd(&current,0);task_discard_file_fd(&current,1);
    puts("PASS pipe2: fixed output contention precedes endpoint publication");
}

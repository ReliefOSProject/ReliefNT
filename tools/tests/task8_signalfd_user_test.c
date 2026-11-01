/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: signalfd user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_signalfd_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "signalfd_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
static uint64_t blocked;static bool contended;static unsigned prepares;
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{assert(task==current && length<=32768);(void)writing;++prepares;
 return contended && address==blocked ? SWAP_STORAGE_CONTENDED : 0;}
int main(void)
{
    current=&readers[0];*current=(struct task){.pid=10,.tgid=10,.kind=TASK_KIND_USER,
        .state=TASK_READY,.blocked_signals=UINT64_MAX};current->limits.sigpending.rlim_cur=32;
    uint64_t mask=1ULL<<34;blocked=(uintptr_t)&mask;contended=true;
    assert(syscall_signalfd(-1,(uintptr_t)&mask,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!descriptor.used);contended=false;
    assert(syscall_signalfd(-1,(uintptr_t)&mask,8,LINUX_SFD_NONBLOCK)==3);
    struct linux_signalfd_siginfo output[2]={0};enqueue(current,false,35,1);
    blocked=(uintptr_t)output;contended=true;
    assert(task_signalfd_read(current,&descriptor,(uintptr_t)output,128)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(kernel_signal_peek(current,mask)==35 && current->signalfd_waiting);
    contended=false;assert(task_signalfd_read(current,&descriptor,(uintptr_t)output,128)==128 && output[0].value_ptr==1);
    enqueue(current,false,35,2);enqueue(current,false,35,3);
    blocked=(uintptr_t)&output[1];contended=true;
    assert(task_signalfd_read(current,&descriptor,(uintptr_t)output,256)==128 && output[0].value_ptr==2);
    assert(kernel_signal_peek(current,mask)==35);
    struct iovec vec[2]={{output,16},{(char *)output+16,112}};
    blocked=(uintptr_t)vec;assert(task_signalfd_readv(current,&descriptor,(uintptr_t)vec,2,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    blocked=(uintptr_t)output+16;
    assert(task_signalfd_readv(current,&descriptor,(uintptr_t)vec,2,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current->signalfd_vectors && kernel_signal_peek(current,mask)==35);
    vec[0].iov_base=(void *)1;contended=false;
    assert(task_signalfd_readv(current,&descriptor,(uintptr_t)vec,2,0)==128 && output[0].value_ptr==3);
    assert(!current->signalfd_vectors && !kernel_signal_peek(current,mask) && !allocations && prepares);
    puts("PASS signalfd real queue: metadata/record contention, retained prefix and signal ownership");
}

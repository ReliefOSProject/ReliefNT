/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: ioctl user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/dispatch.c"
#include "../../kernel/syscall/device/device.c"
#include <reliefnt/swap.h>
static struct task current;
static struct task_file file;
static uint64_t swapped,readonly;
static bool contended;
static unsigned prepares,controls;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_fd(struct task *task,int fd)
{ assert(task==&current && fd==3);return &file; }
int task_pty_ensure_fd(struct task *task,int fd,struct task_pty_fd **out)
{ (void)task;(void)fd;(void)out;assert(0);return -RELIEFOS_EBADF; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing;assert(task==&current && length<=sizeof(struct reliefos_driver_control));++prepares;
    if(address<4096)return -LINUX_EFAULT;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped; }
bool user_range_writable(uint64_t address,uint64_t length)
{ return user_range_ok(address,length) && address!=readonly; }
int driver_manager_control(struct reliefos_driver_control *request)
{ ++controls;request->status=99;return 0; }
int main(void)
{
    current.kind=TASK_KIND_USER;current.cap_effective=1ULL<<CAP_SYS_MODULE;
    int enabled=1;swapped=(uintptr_t)&enabled;contended=true;
    assert(syscall_ioctl_nonblock(&current,3,(uintptr_t)&enabled)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!(file.flags&RELIEFOS_O_NONBLOCK));
    contended=false;assert(!syscall_ioctl_nonblock(&current,3,(uintptr_t)&enabled));
    assert(file.flags&RELIEFOS_O_NONBLOCK);
    struct reliefos_driver_control request={0};swapped=(uintptr_t)&request;contended=true;
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!controls && !request.status);
    contended=false;assert(!syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request));
    assert(controls==1 && request.status==99 && prepares);
    readonly=(uintptr_t)&request;request.status=0;
    assert(syscall_driver_control(RELIEFOS_DRIVER_CONTROL_IOCTL,(uintptr_t)&request)==-LINUX_EFAULT);
    assert(controls==2 && !request.status); /* Existing mutation occurs before failed copyout. */
    puts("PASS FIONBIO and driver control: swapped operands and late output failure");
}

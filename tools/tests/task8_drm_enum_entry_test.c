/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm enum entry.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>
#include <linux/errno.h>
static struct task current;static struct task_file file;
static bool locked,async_io;static unsigned sleeps,releases,calls;
struct task *sched_current_task(void){return &current;}
uint32_t sched_current_pid(void){return current.pid;}
struct task *sched_exec_staged(const struct task *t){(void)t;return NULL;}
void kernel_execution_lock_irqsave(uint64_t *flags){assert(!locked);locked=true;*flags=0;}
void kernel_execution_unlock_irqrestore(uint64_t flags){(void)flags;assert(locked);locked=false;}
void net_tcp_maintenance(void){assert(locked);}
uint32_t vfs_reclaim_mounts(uint32_t n){assert(locked && n==2);return 0;}
uint32_t graphics_service_process_retirements(uint32_t n){assert(locked && n==2);return 0;}
void driver_manager_process_audio_faults(void){assert(locked);}
void usb_process_work(void){assert(locked);}
void input_process_pending(void){assert(locked);}
void task_socket_stage_enter(struct task *t,const struct trap_frame *f){assert(t==&current && f);}
void task_transfer_enter(struct task *t,const struct trap_frame *f){assert(t==&current && f);}
void task_vector_enter(struct task *t,const struct trap_frame *f){assert(t==&current && f);}
void task_lookup_scope_begin(struct task *t,const struct trap_frame *f){assert(t==&current && f);}
void storage_set_io_async_context(bool v){assert(locked);async_io=v;}
uint64_t time_ticks(void){return 100;}
void sched_sleep_current_until(uint64_t t){assert(locked && t==101);current.state=TASK_BLOCKED;++sleeps;}
void storage_release_task_io(uint32_t pid){assert(locked && pid==current.pid);}
void task_release_syscall_file(struct task *t){assert(locked && t==&current && t->syscall_file==&file);t->syscall_file=NULL;++releases;}
bool sched_try_resume_current_user_frame(const struct trap_frame *f){assert(locked && f);return true;}
void arch_set_user_fs(uint64_t fs){(void)fs;assert(locked);}
int64_t syscall_dispatch_regs(struct task *t,uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,
                              uint64_t a3,uint64_t a4,uint64_t a5)
{assert(t==&current && nr==LINUX_SYS_IOCTL && a0==3 && a1==0xc04064a0 && a2==0x1234000 &&
        a3==4 && a4==5 && a5==6 && current.syscall_file==&file && locked && async_io);
 return ++calls==1 ? KERNEL_SYSCALL_DRM_ENUM_WAIT : 0;}
struct task *syscall_dispatch_frame(struct trap_frame *frame);
int main(void)
{
    current=(struct task){.pid=101,.kind=TASK_KIND_USER,.state=TASK_RUNNING,.syscall_file=&file,
                          .syscall_fd=3,.syscall_file_number=LINUX_SYS_IOCTL};
    struct trap_frame f={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=0xc04064a0,.rdx=0x1234000,
                         .r10=4,.r8=5,.r9=6,.rip=0x400002,.cs=0x23};
    assert(!syscall_dispatch_frame(&f));
    assert(!locked && !async_io && current.state==TASK_BLOCKED && sleeps==1 && !releases &&
           f.rip==0x400000 && f.rax==LINUX_SYS_IOCTL && current.restart_syscall==LINUX_SYS_IOCTL+1);
    current.state=TASK_RUNNING;f.rip+=2;
    assert(syscall_dispatch_frame(&f)==&current);
    assert(!locked && !async_io && f.rip==0x400002 && !f.rax && calls==2 && releases==1 && !current.syscall_file);
    puts("PASS real syscall entry: DRM enum marker blocks one tick, retains OFD and resumes");
}

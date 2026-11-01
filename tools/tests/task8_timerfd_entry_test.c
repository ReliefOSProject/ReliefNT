/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Timerfd dispatch owns the admitted OFD across a page-in retry.
 * Scheduler and physical swap completion are modeled; entry and timer code
 * are linked production components.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>
#include <linux/errno.h>
#include "../../kernel/syscall/internal.h"

static struct task current;
static struct task_file original,replacement,*fd_file;
static bool locked,contended;
static uint64_t blocked;
static unsigned sleeps,releases;
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
void storage_set_io_async_context(bool value){assert(locked);(void)value;}
uint64_t time_ticks(void){return 100;}
void sched_sleep_current_until(uint64_t t){assert(locked && t==101);current.state=TASK_BLOCKED;++sleeps;}
void storage_release_task_io(uint32_t pid){assert(locked && pid==current.pid);}
void task_release_syscall_file(struct task *t)
{
    assert(t==&current);
    if(t->syscall_file){assert(t->syscall_file->references>1);--t->syscall_file->references;t->syscall_file=NULL;}
    ++releases;
}
struct task_file *task_file_for_fd(struct task *t,int fd)
{ assert(t==&current && fd==3);return fd_file; }
struct task_file *task_file_for_io(struct task *t,int fd)
{ return t->syscall_file && t->syscall_fd==fd ? t->syscall_file : task_file_for_fd(t,fd); }
struct task_file *task_file_get(struct task_file *f){++f->references;return f;}
bool sched_try_resume_current_user_frame(const struct trap_frame *f){assert(locked && f);return true;}
void arch_set_user_fs(uint64_t fs){(void)fs;assert(locked);}
int swap_prepare_user_range(struct task *t,uint64_t address,uint64_t size,bool output)
{ assert(t==&current && size<=32);(void)output;return contended && address==blocked ? SWAP_STORAGE_CONTENDED : 0; }
bool user_range_ok(uint64_t address,uint64_t size){return address>=4096 && size<=UINT64_MAX-address;}
bool user_range_writable(uint64_t address,uint64_t size){return user_range_ok(address,size);}
int time_clock_get(int32_t clock,struct linux_timespec *value)
{ (void)clock;*value=(struct linux_timespec){0};return 0; }
int64_t syscall_timerfd_settime(uint64_t fd,uint32_t flags,uint64_t value,uint64_t old);
int64_t syscall_timerfd_gettime(uint64_t fd,uint64_t value);
int64_t syscall_dispatch_regs(struct task *t,uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,
                              uint64_t a3,uint64_t a4,uint64_t a5)
{
    assert(t==&current && locked);(void)a4;(void)a5;
    return nr==LINUX_SYS_TIMERFD_SETTIME ? syscall_timerfd_settime(a0,(uint32_t)a1,a2,a3)
                                        : syscall_timerfd_gettime(a0,a1);
}
struct task *syscall_dispatch_frame(struct trap_frame *frame);

int main(void)
{
    current=(struct task){.pid=101,.kind=TASK_KIND_USER,.state=TASK_RUNNING};
    struct task_timerfd a={.expiry_tick=150},b={.expiry_tick=190};
    original=(struct task_file){.used=1,.references=1,.flags=TASK_FILE_FLAG_TIMERFD,.aux=(uintptr_t)&a};
    replacement=(struct task_file){.used=1,.references=1,.flags=TASK_FILE_FLAG_TIMERFD,.aux=(uintptr_t)&b};
    struct linux_itimerspec input={.it_value={.tv_sec=2}},output={0};
    struct trap_frame f={.rax=LINUX_SYS_TIMERFD_SETTIME,.rdi=3,.rdx=(uintptr_t)&input,
                         .r10=(uintptr_t)&output,.rip=0x400002,.cs=0x23};
    fd_file=&original;blocked=(uintptr_t)&output;contended=true;
    assert(!syscall_dispatch_frame(&f) && current.state==TASK_BLOCKED && a.expiry_tick==150);
    fd_file=&replacement;contended=false;current.state=TASK_RUNNING;f.rip+=2;
    assert(syscall_dispatch_frame(&f)==&current && !f.rax);
    assert(a.expiry_tick==300 && b.expiry_tick==190 && original.references==1);
    f=(struct trap_frame){.rax=LINUX_SYS_TIMERFD_GETTIME,.rdi=3,.rsi=(uintptr_t)&output,
                          .rip=0x400002,.cs=0x23};
    fd_file=&original;contended=true;current.state=TASK_RUNNING;
    assert(!syscall_dispatch_frame(&f));
    replacement.flags=TASK_FILE_FLAG_DEV_NULL;fd_file=&replacement;contended=false;
    current.state=TASK_RUNNING;f.rip+=2;
    assert(syscall_dispatch_frame(&f)==&current && !f.rax && output.it_value.tv_sec==2);
    assert(original.references==1 && replacement.references==1 && sleeps==2 && releases>=2);
    f=(struct trap_frame){.rax=LINUX_SYS_TIMERFD_GETTIME,.rdi=3,.rsi=(uintptr_t)&output,
                          .rip=0x400002,.cs=0x23};
    fd_file=&original;contended=true;current.state=TASK_RUNNING;
    assert(!syscall_dispatch_frame(&f) && original.references==2);
    fd_file=&replacement;task_release_syscall_file(&current);
    assert(original.references==1 && replacement.references==1 && !current.syscall_file);
    puts("PASS timerfd frame retains original OFD across descriptor reuse");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: timerfd user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/event/timerfd.c"
#include <reliefnt/swap.h>

static struct task current;
static struct task_file file;
static struct task_timerfd timer;
static uint64_t swapped;
static uint64_t unmapped;
static bool contended;
static unsigned prepares, allocations;
static char saved_name[250];
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_fd(struct task *task,int fd)
{ assert(task==&current); return fd==3 ? &file : NULL; }
struct task_file *task_file_for_io(struct task *task,int fd)
{ return task->syscall_file && task->syscall_fd==fd ? task->syscall_file : task_file_for_fd(task,fd); }
uint64_t time_ticks(void) { return 100; }
int time_clock_get(int32_t clock,struct linux_timespec *value)
{ (void)clock; *value=(struct linux_timespec){0}; return 0; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing; assert(task==&current && length<=250); ++prepares;
    if(address<4096 || address==unmapped) return -RELIEFOS_EFAULT;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped && address!=unmapped; }
bool user_range_writable(uint64_t address,uint64_t length)
{ return user_range_ok(address,length); }
size_t user_strlen(const char *value,size_t max)
{ size_t n=0;while(n<max && value[n])++n;return n; }
#ifndef TASK8_REAL_PATH
int copy_user_path(char *dst,uint32_t capacity,uint64_t address)
{
    int rc=swap_prepare_user_range(&current,address,capacity,false);
    if(rc==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
    if(rc<0 || !user_range_ok(address,capacity))return -RELIEFOS_EFAULT;
    size_t n=user_strlen((const char *)(uintptr_t)address,capacity);
    if(n==capacity)return -RELIEFOS_ENAMETOOLONG;
    memcpy(dst,(const void *)(uintptr_t)address,n+1);return 0;
}
#endif
int alloc_task_fd(struct task *task,const struct storage_node *node,uint32_t flags,const char *path)
{ (void)node;(void)flags;(void)path;assert(task==&current);++allocations;return 3; }
int task_shm_attach(struct task_file *f,uint32_t flags)
{ (void)flags;assert(f==&file);return 0; }
int task_shm_truncate(struct task_file *f,uint64_t size)
{ assert(f==&file && !size);return 0; }
int task_shm_set_name(struct task_file *f,const char *name,size_t length)
{ assert(f==&file && length<sizeof(saved_name));memcpy(saved_name,name,length+1);return 0; }
void task_discard_file_fd(struct task *task,int fd) { (void)task;(void)fd;assert(0); }
int main(void)
{
    current.kind=TASK_KIND_USER;
    timer=(struct task_timerfd){.expiry_tick=150,.interval_ticks=10};
    file.flags=TASK_FILE_FLAG_TIMERFD;file.aux=(uintptr_t)&timer;
    struct linux_itimerspec input={.it_value={.tv_sec=2}},output={0};
    swapped=(uintptr_t)&input;contended=true;
    assert(syscall_timerfd_settime(3,0,(uintptr_t)&input,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(timer.expiry_tick==150 && swapped);
    contended=false;swapped=(uintptr_t)&output;contended=true;
    assert(syscall_timerfd_settime(3,0,(uintptr_t)&input,(uintptr_t)&output)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(timer.expiry_tick==150 && swapped);
    contended=false;
    assert(!syscall_timerfd_settime(3,0,(uintptr_t)&input,(uintptr_t)&output));
    assert(timer.expiry_tick==300 && !swapped && prepares);
    timer.expiry_tick=150;
    assert(syscall_timerfd_settime(3,0,(uintptr_t)&input,1)==-RELIEFOS_EFAULT);
    assert(timer.expiry_tick==300); /* Preserve existing late invalid-output behavior. */
    swapped=(uintptr_t)&output;contended=true;timer.expiry_tick=90;
    assert(syscall_timerfd_gettime(3,(uintptr_t)&output)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(timer.expiry_tick==90);
    contended=false;assert(!syscall_timerfd_gettime(3,(uintptr_t)&output));
    assert(!timer.expiry_tick && timer.expirations==1);
    char name[250]="swap-ready";swapped=(uintptr_t)name;contended=true;
    assert(syscall_memfd_create((uintptr_t)name,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!allocations && swapped);
    contended=false;assert(syscall_memfd_create((uintptr_t)name,0)==3);
    assert(!strcmp(saved_name,"swap-ready") && allocations==1);
    memset(name,'x',sizeof(name));assert(syscall_memfd_create((uintptr_t)name,0)==-RELIEFOS_EINVAL);
    puts("PASS timerfd and memfd: bounded swapped operands, retry and late EFAULT");
    return 0;
}

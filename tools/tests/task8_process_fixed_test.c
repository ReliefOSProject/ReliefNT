/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: process fixed.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/process/process.c"
#include <reliefnt/swap.h>
static struct task current;
static uint64_t swapped;
static bool contended;
static unsigned prepares;
struct task *sched_current_task(void) { return &current; }
struct task *sched_find(uint32_t pid) { return pid==current.pid ? &current : NULL; }
uint32_t sched_current_pid(void) { return current.pid; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing;assert(task==&current && length<=4096);++prepares;
    if(address<4096)return -LINUX_EFAULT;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped; }
bool user_range_writable(uint64_t address,uint64_t length) { return user_range_ok(address,length); }
int time_clock_get(int32_t clock,struct linux_timespec *value)
{ (void)clock;*value=(struct linux_timespec){.tv_sec=7,.tv_nsec=123000};return 0; }
int time_clock_getres(int32_t clock,struct linux_timespec *value)
{ (void)clock;*value=(struct linux_timespec){.tv_nsec=10000000};return 0; }
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=42;current.robust_list=0x1234;
    struct linux_timespec ts={0};swapped=(uintptr_t)&ts;contended=true;
    assert(syscall_process_control(LINUX_SYS_CLOCK_GETTIME,0,(uintptr_t)&ts,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!ts.tv_sec);contended=false;
    assert(!syscall_process_control(LINUX_SYS_CLOCK_GETTIME,0,(uintptr_t)&ts,0,0) && ts.tv_sec==7);
    uint64_t head=0,len=0;swapped=(uintptr_t)&len;contended=true;
    assert(syscall_process_control(LINUX_SYS_GET_ROBUST_LIST,0,(uintptr_t)&head,(uintptr_t)&len,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!head && !len);contended=false;
    assert(!syscall_process_control(LINUX_SYS_GET_ROBUST_LIST,0,(uintptr_t)&head,(uintptr_t)&len,0));
    assert(head==0x1234 && len==sizeof(struct linux_robust_list_head));
    int64_t timeval[2]={0};int32_t zone[2]={1,1};swapped=(uintptr_t)zone;contended=true;
    assert(syscall_process_control(LINUX_SYS_GETTIMEOFDAY,(uintptr_t)timeval,(uintptr_t)zone,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!timeval[0] && zone[0]==1);contended=false;
    assert(!syscall_process_control(LINUX_SYS_GETTIMEOFDAY,(uintptr_t)timeval,(uintptr_t)zone,0,0));
    assert(timeval[0]==7 && !zone[0]);
    char name[16]="worker";swapped=(uintptr_t)name;contended=true;
    assert(syscall_process_prctl(LINUX_PR_SET_NAME,(uintptr_t)name,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!current.name_storage[0]);contended=false;
    assert(!syscall_process_prctl(LINUX_PR_SET_NAME,(uintptr_t)name,0,0,0));
    assert(!strcmp(current.name_storage,"worker") && prepares);
    puts("PASS process fixed operands: clocks, multi-output atomicity, robust list and prctl");
}

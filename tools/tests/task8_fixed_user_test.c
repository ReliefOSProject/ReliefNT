/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: fixed user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/process/resources.c"
#include <reliefnt/swap.h>
#include <reliefnt/memory_stats.h>

static struct task current;
static uint64_t swapped;
static bool contended;
static unsigned prepares;
struct task *sched_current_task(void) { return &current; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && writing && length<=32768);++prepares;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_writable(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped; }
uint64_t time_ticks(void) { return 123; }
uint64_t sched_task_cpu_ticks(const struct task *task,bool system) { (void)system;assert(task==&current);return 2; }
uint64_t address_space_user_resident_kib(const struct address_space *as) { (void)as;return 4; }
void sched_load_averages(uint64_t out[3]) { out[0]=out[1]=out[2]=0; }
uint64_t mm_total_memory_kib(void) { return 100; }
uint64_t mm_free_memory_kib(void) { return 50; }
static struct memory_snapshot expected_memory = {.total_kib=100, .free_kib=50,
    .swap_total_kib=128, .swap_free_kib=64};
void memory_snapshot_get(struct memory_snapshot *out) { *out=expected_memory; }
void sched_task_counts(uint32_t *count,uint32_t *running,uint32_t *blocked,uint32_t *stopped)
{ (void)running;(void)blocked;(void)stopped;*count=1; }
int main(void)
{
    current.kind=TASK_KIND_USER;
    struct linux_sysinfo info={0};swapped=(uintptr_t)&info;contended=true;
    assert(syscall_sysinfo((uintptr_t)&info)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!info.totalram && swapped);
    contended=false;
    assert(!syscall_sysinfo((uintptr_t)&info) && info.totalram==100 && !swapped && prepares);
    assert(info.freeram==50 && info.totalswap==128 && info.freeswap==64 && info.mem_unit==1024);
    expected_memory=(struct memory_snapshot){.total_kib=1ULL<<42,.free_kib=1ULL<<41,
        .swap_total_kib=1ULL<<40,.swap_free_kib=(1ULL<<39)+4};
    assert(!syscall_sysinfo((uintptr_t)&info) && info.totalram==(1ULL<<42) &&
           info.freeram==(1ULL<<41) && info.totalswap==(1ULL<<40) &&
           info.freeswap==(1ULL<<39)+4 && info.mem_unit==1024);
    struct linux_rusage usage={0};swapped=(uintptr_t)&usage;
    assert(!syscall_getrusage(0,(uintptr_t)&usage) && usage.ru_maxrss==4);
    struct linux_tms times={0};swapped=(uintptr_t)&times;
    assert(syscall_times((uintptr_t)&times)==123 && times.tms_utime==2);
    assert(syscall_sysinfo(1)==-RELIEFOS_EFAULT);
    puts("PASS fixed user outputs: bounded swap preparation and contention");
}

/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "modules/scheduler.h"

uint32_t smp_current_cpu(void) { return 0; }
uint32_t smp_cpu_count(void) { return 4; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 4; }
uint64_t time_uptime_us(void) { return scheduler_ticks * 10000; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
int kernel_signal_fatal_pending(const struct task *task)
{ (void)task; return 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

int main(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1024;
    unsigned iterations = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 20000;
    assert(count && count <= 4096 && iterations && iterations <= 10000000);
    sched_init();
    struct task *members = calloc(count, sizeof(*members));
    struct task **table = calloc(count, sizeof(*table));
    assert(members && table);
    tasks = table; task_count = count;
    for (unsigned i = 0; i < count; ++i) {
        table[i] = &members[i];
        members[i].pid = members[i].tgid = i + 1;
        members[i].kind = TASK_KIND_USER;
        members[i].state = TASK_BLOCKED;
        members[i].running_cpu = SCHED_CPU_NONE;
    }
    struct timespec start, end;
    assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start));
    for (unsigned i = 0; i < iterations; ++i) sched_on_tick();
    assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end));
    uint64_t elapsed_ns = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL;
    elapsed_ns += (int64_t)end.tv_nsec - start.tv_nsec;
    assert(scheduler_ticks == iterations);
    printf("tasks=%u ticks=%u cpu_ns=%llu ns_per_tick=%.2f\n", count, iterations,
           (unsigned long long)elapsed_ns, (double)elapsed_ns / iterations);
    free(table); free(members);
    return 0;
}

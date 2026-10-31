#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <reliefnt/lock.h>
#include <reliefnt/syscall.h>

/* These declarations describe the observable collector contract; weak binding
 * makes the pre-implementation test fail explicitly instead of failing to link. */
#define BINS 32
struct profile_stats {
    uint64_t acquisitions, contentions, wait_total, wait_max;
    uint64_t hold_total, hold_max, irq_total, irq_max;
    uint64_t wait_hist[BINS], hold_hist[BINS], irq_hist[BINS];
    uint64_t clock_errors;
};
extern int lock_profile_control(const char *, uint32_t) __attribute__((weak));
extern int lock_profile_read(uint64_t, void *, uint32_t, uint32_t *) __attribute__((weak));
extern uint32_t lock_profile_syscall_scope(uint64_t) __attribute__((weak));
extern int lock_profile_snapshot(uint32_t, uint32_t, struct profile_stats *) __attribute__((weak));
extern void kernel_execution_lock_scoped_irqsave(uint64_t *, uint32_t) __attribute__((weak));
extern bool kernel_execution_try_read_lock_scoped_irqsave(uint64_t *, uint32_t) __attribute__((weak));

enum { GENERIC, SYSCALL_IO, SYSCALL_PROCESS, SYSCALL_MEMORY, SYSCALL_OTHER, FAULT, SCHED, RESUMED, SCOPES };
static _Thread_local uint32_t cpu;
static _Thread_local uint64_t irq = 1u << 9, clock_value;
static _Thread_local unsigned clock_calls, scripted_count, scripted_pos;
static _Thread_local uint64_t scripted[3];
static atomic_int polling, entered, release_worker, active_readers, active_writers;
static atomic_int fifo_entered[2], fifo_release[2];
uint32_t smp_current_cpu(void) { return cpu; }
uint32_t smp_cpu_count(void) { return 4; }
void smp_membarrier_poll(void) { atomic_fetch_or(&polling, 1u << cpu); }
uint64_t kernel_irq_save(void) { uint64_t f = irq; irq = 0; return f; }
void kernel_irq_restore(uint64_t f) { irq = f; }
uint64_t lock_profile_test_clock(void)
{
    ++clock_calls;
    if (scripted_pos < scripted_count) return scripted[scripted_pos++];
    clock_value += 10; return clock_value;
}
static void use_clock(uint64_t a, uint64_t b, uint64_t c)
{ scripted[0] = a; scripted[1] = b; scripted[2] = c; scripted_count = 3; scripted_pos = 0; }

static struct profile_stats stats(uint32_t c, uint32_t scope)
{
    struct profile_stats result;
    memset(&result, 0xa5, sizeof(result));
    assert(!lock_profile_snapshot(c, scope, &result));
    return result;
}
static void command(const char *s) { assert(!lock_profile_control(s, (uint32_t)strlen(s))); }
static void reset(void) { command("reset"); }
static void wait_for(atomic_int *value)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    while (!atomic_load(value)) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        assert(b.tv_sec - a.tv_sec < 3 && "gate progress timed out");
    }
}

static void test_syscall_classification(void)
{
    const uint64_t io[] = {LINUX_SYS_READ, LINUX_SYS_WRITE, LINUX_SYS_PREAD64, LINUX_SYS_PWRITE64,
        LINUX_SYS_PREADV, LINUX_SYS_PWRITEV, LINUX_SYS_OPENAT, LINUX_SYS_CLOSE, LINUX_SYS_LSEEK,
        LINUX_SYS_FSTAT, LINUX_SYS_NEWFSTATAT, LINUX_SYS_STATX, LINUX_SYS_GETDENTS64,
        LINUX_SYS_FSYNC, LINUX_SYS_RENAMEAT2, LINUX_SYS_READLINKAT};
    for (unsigned i = 0; i < sizeof(io) / sizeof(io[0]); ++i)
        assert(lock_profile_syscall_scope(io[i]) == SYSCALL_IO);
    assert(lock_profile_syscall_scope(LINUX_SYS_EXECVE) == SYSCALL_PROCESS);
    assert(lock_profile_syscall_scope(LINUX_SYS_MMAP) == SYSCALL_MEMORY);
    assert(lock_profile_syscall_scope(UINT64_MAX) == SYSCALL_OTHER);
}
static void test_disabled_and_recursion(void)
{
    uint64_t f, nested;
    unsigned before = clock_calls;
    kernel_execution_lock_irqsave(&f);
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    assert(stats(0, GENERIC).acquisitions == 0);
    command("1"); reset();
    clock_value = 0; before = clock_calls;
    kernel_execution_lock_irqsave(&f);
    kernel_execution_lock_irqsave(&nested);
    kernel_execution_unlock_irqrestore(nested);
    assert(clock_calls == before + 2);
    kernel_execution_unlock_irqrestore(f);
    struct profile_stats s = stats(0, GENERIC);
    assert(clock_calls == before + 3);
    assert(s.acquisitions == 1 && s.contentions == 0);
    assert(s.wait_total == 10 && s.wait_max == 10);
    assert(s.hold_total == 10 && s.hold_max == 10);
    assert(s.irq_total == 20 && s.irq_max == 20);
    assert(s.wait_hist[3] == 1 && s.hold_hist[3] == 1 && s.irq_hist[4] == 1);
    assert(irq == (1u << 9));
}

static void test_control_epoch_and_scope(void)
{
    uint64_t f;
    reset();
    kernel_execution_lock_scoped_irqsave(&f, SYSCALL_IO);
    reset();
    unsigned before = clock_calls;
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    assert(stats(0, SYSCALL_IO).acquisitions == 0);
    kernel_execution_lock_scoped_irqsave(&f, SYSCALL_IO);
    command("0"); before = clock_calls;
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    struct profile_stats s = stats(0, SYSCALL_IO);
    assert(s.acquisitions == 1 && s.hold_total == 0);
    kernel_execution_lock_irqsave(&f);
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    command("1");
    assert(stats(0, SYSCALL_IO).acquisitions == 1);
    kernel_execution_lock_scoped_irqsave(&f, SYSCALL_IO);
    command("0"); command("1"); before = clock_calls;
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before && stats(0, SYSCALL_IO).hold_total == 0);
    assert(lock_profile_control("", 0) < 0);
    assert(lock_profile_control("1x", 2) < 0);
    assert(lock_profile_control("reset\nextra", 11) < 0);
    assert(lock_profile_control(" 1", 2) < 0);
    assert(!lock_profile_control("0\n", 2));
    assert(!lock_profile_control("1\n", 2));
    assert(lock_profile_snapshot(64, GENERIC, &s) < 0);
    assert(lock_profile_snapshot(0, SCOPES, &s) < 0);
    reset(); before = clock_calls;
    kernel_execution_lock_scoped_irqsave(&f, SCOPES);
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    cpu = 64;
    kernel_execution_lock_irqsave(&f);
    kernel_execution_unlock_irqrestore(f);
    assert(clock_calls == before);
    cpu = 0;
    assert(stats(0, GENERIC).acquisitions == 0);
}

static void test_rendered_counters_and_offsets(void)
{
    uint64_t f;
    char text[8192], part[40];
    uint32_t count, n;
    reset(); clock_value = 0;
    kernel_execution_lock_scoped_irqsave(&f, SYSCALL_IO);
    kernel_execution_unlock_irqrestore(f);
    assert(!lock_profile_read(0, text, sizeof(text) - 1, &count)); text[count] = 0;
    assert(strstr(text, "reliefnt_lock_stats 1 unit=tsc_cycles enabled=1"));
    assert(strstr(text, "cpu=0 scope=syscall_io acquisitions=1 contentions=0 wait_total=10 wait_max=10 hold_total=10 hold_max=10 irq_total=20 irq_max=20 clock_errors=0"));
    assert(strstr(text, "wait_hist=0,0,0,1,"));
    assert(strstr(text, "irq_hist=0,0,0,0,1,"));
    assert(!lock_profile_read(9, part, sizeof(part), &n) && n == sizeof(part));
    assert(!memcmp(text + 9, part, n));
    assert(!lock_profile_read(UINT64_MAX, part, sizeof(part), &n) && n == 0);
}

static void test_backward_clock_and_saturation(void)
{
    uint64_t f;
    reset();
    use_clock(100, 90, 80);
    kernel_execution_lock_irqsave(&f); kernel_execution_unlock_irqrestore(f);
    struct profile_stats s = stats(0, GENERIC);
    assert(s.acquisitions == 1 && s.clock_errors == 3);
    assert(s.wait_total == 0 && s.hold_total == 0 && s.irq_total == 0);
    assert(s.wait_hist[0] == 0 && s.hold_hist[0] == 0 && s.irq_hist[0] == 0);
    reset();
    use_clock(0, UINT64_MAX, UINT64_MAX);
    kernel_execution_lock_irqsave(&f); kernel_execution_unlock_irqrestore(f);
    use_clock(0, 2, 3);
    kernel_execution_lock_irqsave(&f); kernel_execution_unlock_irqrestore(f);
    s = stats(0, GENERIC);
    assert(s.wait_total == UINT64_MAX && s.irq_total == UINT64_MAX);
    assert(s.wait_max == UINT64_MAX && s.wait_hist[31] == 1 && s.clock_errors == 0);
    scripted_count = scripted_pos = 0;
}

static void test_suspend_migration_and_invalid(void)
{
    uint64_t f, nested;
    struct kernel_execution_suspend_token token = {0};
    reset(); clock_value = 0;
    kernel_execution_lock_irqsave(&f);
    kernel_execution_lock_irqsave(&nested);
    assert(kernel_execution_suspend_irqrestore(&token) == -35 && !token.active);
    kernel_execution_unlock_irqrestore(nested);
    assert(kernel_execution_suspend_irqrestore(&token) == 0 && token.active);
    struct profile_stats s = stats(0, GENERIC);
    assert(s.acquisitions == 1 && s.hold_total == 10 && s.irq_total == 20);
    assert(kernel_execution_suspend_irqrestore(&token) == -22);
    token.wait_exit_irq_flags = 0x1234;
    struct kernel_execution_suspend_token before = token;
    kernel_execution_lock_irqsave(&nested);
    assert(kernel_execution_resume_irqsave(&token) == -35);
    assert(!memcmp(&before, &token, sizeof(token)));
    kernel_execution_unlock_irqrestore(nested);
    cpu = 1; clock_value = 1000;
    assert(kernel_execution_resume_irqsave(&token) == 0 && !token.active);
    assert(kernel_execution_resume_irqsave(&token) == -22);
    kernel_execution_unlock_irqrestore(f);
    s = stats(1, RESUMED);
    assert(s.acquisitions == 1 && s.wait_total == 10 && s.hold_total == 10 && s.irq_total == 20);
    assert(kernel_execution_suspend_irqrestore(&token) == -1);
    kernel_execution_lock_irqsave(&f);
    assert(kernel_execution_suspend_irqrestore(&token) == 0);
    cpu = 2; clock_value = 2000;
    assert(kernel_execution_resume_irqsave(&token) == 0);
    assert(kernel_execution_suspend_irqrestore(&token) == 0);
    cpu = 3; clock_value = 3000;
    assert(kernel_execution_resume_irqsave(&token) == 0);
    kernel_execution_unlock_irqrestore(f);
    assert(stats(2, RESUMED).hold_total == 10 && stats(2, RESUMED).irq_total == 20);
    assert(stats(3, RESUMED).hold_total == 10 && stats(3, RESUMED).irq_total == 20);
    cpu = 0;
    assert(irq == (1u << 9));
}

static void *writer(void *unused)
{
    (void)unused; cpu = 1; uint64_t f;
    kernel_execution_lock_scoped_irqsave(&f, SCHED);
    atomic_store(&entered, 1); wait_for(&release_worker);
    kernel_execution_unlock_irqrestore(f);
    return NULL;
}
static void *reader(void *unused)
{
    (void)unused; cpu = 2; uint64_t f;
    assert(kernel_execution_try_read_lock_scoped_irqsave(&f, FAULT));
    atomic_store(&entered, 1); wait_for(&release_worker);
    kernel_execution_read_unlock_irqrestore(f);
    return NULL;
}
static void test_reader_overlap_and_writer_contention(void)
{
    pthread_t t; uint64_t f, other;
    reset(); atomic_store(&entered, 0); atomic_store(&release_worker, 0);
    assert(kernel_execution_try_read_lock_scoped_irqsave(&f, FAULT));
    assert(!pthread_create(&t, NULL, reader, NULL)); wait_for(&entered);
    atomic_store(&release_worker, 1); assert(!pthread_join(t, NULL));
    kernel_execution_read_unlock_irqrestore(f);
    assert(stats(0, FAULT).acquisitions == 1 && stats(2, FAULT).acquisitions == 1);

    reset(); atomic_store(&entered, 0); atomic_store(&release_worker, 0); atomic_store(&polling, 0);
    assert(kernel_execution_try_read_lock_scoped_irqsave(&f, FAULT));
    assert(!pthread_create(&t, NULL, writer, NULL)); wait_for(&polling);
    assert(!kernel_execution_try_read_lock_scoped_irqsave(&other, FAULT));
    kernel_execution_read_unlock_irqrestore(f); wait_for(&entered);
    assert(!kernel_execution_try_read_lock_scoped_irqsave(&other, FAULT));
    atomic_store(&release_worker, 1); assert(!pthread_join(t, NULL));
    assert(stats(0, FAULT).acquisitions == 1);
    assert(stats(1, SCHED).acquisitions == 1 && stats(1, SCHED).contentions == 1);
}
static void *fifo_writer(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    cpu = id + 1;
    uint64_t f;
    kernel_execution_lock_scoped_irqsave(&f, SCHED);
    atomic_store(&fifo_entered[id], 1);
    wait_for(&fifo_release[id]);
    kernel_execution_unlock_irqrestore(f);
    return NULL;
}
static void test_fifo_writer_admission(void)
{
    pthread_t first, second;
    uint64_t f;
    reset(); atomic_store(&polling, 0);
    for (unsigned i = 0; i < 2; ++i) {
        atomic_store(&fifo_entered[i], 0); atomic_store(&fifo_release[i], 0);
    }
    assert(kernel_execution_try_read_lock_scoped_irqsave(&f, FAULT));
    assert(!pthread_create(&first, NULL, fifo_writer, (void *)0));
    while (!(atomic_load(&polling) & (1u << 1))) {}
    assert(!pthread_create(&second, NULL, fifo_writer, (void *)1));
    while (!(atomic_load(&polling) & (1u << 2))) {}
    kernel_execution_read_unlock_irqrestore(f);
    wait_for(&fifo_entered[0]);
    assert(!atomic_load(&fifo_entered[1]));
    atomic_store(&fifo_release[0], 1); wait_for(&fifo_entered[1]);
    atomic_store(&fifo_release[1], 1);
    assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
    assert(stats(1, SCHED).contentions == 1 && stats(2, SCHED).contentions == 1);
}
static void *stress(void *arg)
{
    cpu = (uintptr_t)arg;
    for (unsigned i = 0; i < 3000; ++i) {
        uint64_t f;
        if (i % 3 && kernel_execution_try_read_lock_scoped_irqsave(&f, FAULT)) {
            atomic_fetch_add(&active_readers, 1); assert(!atomic_load(&active_writers));
            atomic_fetch_sub(&active_readers, 1); kernel_execution_read_unlock_irqrestore(f);
        } else {
            kernel_execution_lock_scoped_irqsave(&f, SCHED);
            assert(!atomic_fetch_add(&active_writers, 1)); assert(!atomic_load(&active_readers));
            atomic_fetch_sub(&active_writers, 1); kernel_execution_unlock_irqrestore(f);
        }
    }
    return NULL;
}
static void test_threaded_stress(void)
{
    pthread_t threads[4]; reset();
    for (uintptr_t i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, stress, (void *)i));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    uint64_t total = 0;
    for (uint32_t i = 0; i < 4; ++i) total += stats(i, FAULT).acquisitions + stats(i, SCHED).acquisitions;
    assert(total == 12000);
}
int main(void)
{
    assert(lock_profile_control && lock_profile_snapshot && lock_profile_read && lock_profile_syscall_scope && kernel_execution_lock_scoped_irqsave &&
           kernel_execution_try_read_lock_scoped_irqsave && "execution-gate profiler is missing");
    test_syscall_classification(); test_disabled_and_recursion(); test_control_epoch_and_scope(); test_rendered_counters_and_offsets(); test_backward_clock_and_saturation();
    test_suspend_migration_and_invalid(); test_reader_overlap_and_writer_contention();
    test_fifo_writer_admission(); test_threaded_stress();
    puts("PASS execution-gate profiling: recursion, epochs, migration, contention and threaded gate");
}

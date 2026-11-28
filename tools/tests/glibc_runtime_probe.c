/* SPDX-License-Identifier: Apache-2.0 */
/* Compile against an unmodified glibc SDK and run on Linux or ReliefNT. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <gnu/libc-version.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned failures;
static volatile sig_atomic_t signal_seen;
static volatile sig_atomic_t signal_xstate_ok;
static const uint64_t avx_pattern[4] = {
    UINT64_C(0x123456789abcdef0), UINT64_C(0x876543210fedcba9),
    UINT64_C(0x7f61524334251607), UINT64_C(0xfedcba9876543210)
};

static void check(const char *name, int passed)
{
    printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    failures += !passed;
}

static void signal_handler(int signo)
{
    (void)signo;
    signal_seen++;
}

static int current_cpu_matches(void)
{
    unsigned cpu;
    return syscall(SYS_getcpu, &cpu, NULL, NULL) == 0 &&
        sched_getcpu() == (int)cpu;
}

static void *thread_cpu_probe(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)current_cpu_matches();
}

static void test_rseq(void)
{
    cpu_set_t original, pinned;
    int ret = sched_getaffinity(0, sizeof(original), &original);
    check("rseq affinity query", ret == 0);
    if (ret) return;
    check("glibc rseq registration", __rseq_size >= 20);
    unsigned checked = 0;
    int matched = 1;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE && checked < 2; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        CPU_ZERO(&pinned);
        CPU_SET(cpu, &pinned);
        if (sched_setaffinity(0, sizeof(pinned), &pinned) != 0) {
            matched = 0;
            break;
        }
        matched &= current_cpu_matches() && sched_getcpu() == (int)cpu;
        checked++;
    }
    check("rseq CPU publication after migration", checked != 0 && matched);
    check("restore CPU affinity", sched_setaffinity(0, sizeof(original), &original) == 0);
    pthread_t thread;
    void *result = NULL;
    ret = pthread_create(&thread, NULL, thread_cpu_probe, NULL);
    if (!ret) ret = pthread_join(thread, &result);
    check("glibc child thread rseq registration", ret == 0 && result == (void *)1);
    pid_t child = fork();
    if (!child) _exit(current_cpu_matches() ? 0 : 1);
    int status = 0;
    check("fork inherits rseq registration", child > 0 && waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (__rseq_size < 20) return;
    struct rseq *area = (struct rseq *)((char *)__builtin_thread_pointer() + __rseq_offset);
    struct sigaction action = { .sa_handler = signal_handler };
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    struct itimerval timer = { .it_value = { .tv_usec = 50000 } };
    setitimer(ITIMER_REAL, &timer, NULL);
    unsigned aborted;
    /* A real rseq critical section. Preemption or the timer signal must
     * redirect execution to the signed abort target and clear rseq_cs. */
    __asm__ volatile(
        ".pushsection __rseq_cs,\"aw\"\n\t"
        ".balign 32\n\t"
        "rseq_descriptor%=:\n\t"
        ".long 0, 0\n\t"
        ".quad rseq_start%=, rseq_post%=-rseq_start%=, rseq_abort%=\n\t"
        ".popsection\n\t"
        "lea rseq_descriptor%=(%%rip), %%r11\n\t"
        "mov %%r11, 8(%[area])\n\t"
        "xor %[aborted], %[aborted]\n\t"
        "rseq_start%=:\n\tpause\n\tjmp rseq_start%=\n\t"
        "rseq_post%=:\n\tjmp rseq_done%=\n\t"
        ".long 0x53053053\n\t"
        "rseq_abort%=:\n\tmov $1, %[aborted]\n\t"
        "rseq_done%=:\n\t"
        : [aborted] "=&r"(aborted) : [area] "r"(area) : "r11", "cc", "memory");
    timer = (struct itimerval){0};
    setitimer(ITIMER_REAL, &timer, NULL);
    check("rseq interrupted section abort and pointer clearing", aborted == 1 && area->rseq_cs == 0);
}

static void test_cpu_clocks(void)
{
    clockid_t clock;
    struct timespec now, resolution;
    int ret = pthread_getcpuclockid(pthread_self(), &clock);
    check("pthread_getcpuclockid and encoded thread clock", ret == 0 &&
          clock_gettime(clock, &now) == 0 && clock_getres(clock, &resolution) == 0);
    ret = clock_getcpuclockid(getpid(), &clock);
    check("clock_getcpuclockid and encoded process clock", ret == 0 &&
          clock_gettime(clock, &now) == 0 && clock_getres(clock, &resolution) == 0);
    clockid_t invalid = (clockid_t)((~(uint32_t)getpid() << 3) | 3u);
    errno = 0;
    check("invalid encoded CPU clock", clock_gettime(invalid, &now) == -1 && errno == EINVAL);
}

struct pi_context {
    pthread_mutex_t mutex;
    atomic_int ready;
    atomic_int release;
    int result;
    int timed;
    int owner;
};

static void *pi_worker(void *opaque)
{
    struct pi_context *context = opaque;
    atomic_store(&context->ready, 1);
    if (context->timed) {
        struct timespec deadline;
        clockid_t clock = context->timed == 1 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        clock_gettime(clock, &deadline);
        deadline.tv_nsec += 80000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_nsec -= 1000000000;
            deadline.tv_sec++;
        }
        context->result = pthread_mutex_clocklock(&context->mutex, clock, &deadline);
    } else {
        context->result = pthread_mutex_lock(&context->mutex);
    }
    if (context->owner) {
        atomic_store(&context->ready, 2);
        while (!atomic_load(&context->release)) sched_yield();
        return NULL;
    }
    if (context->result == EOWNERDEAD) pthread_mutex_consistent(&context->mutex);
    if (!context->result || context->result == EOWNERDEAD)
        pthread_mutex_unlock(&context->mutex);
    return NULL;
}

static void wait_ready(struct pi_context *context, int state)
{
    while (atomic_load(&context->ready) < state) sched_yield();
}

static void test_pi_mutex(void)
{
    pthread_mutexattr_t attr;
    struct pi_context context = {0};
    int ret = pthread_mutexattr_init(&attr);
    if (!ret) ret = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (!ret) ret = pthread_mutex_init(&context.mutex, &attr);
    check("glibc PI mutex initialization", ret == 0);
    if (ret) {
        pthread_mutexattr_destroy(&attr);
        return;
    }
    struct sigaction action = { .sa_handler = signal_handler };
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, NULL);
    pthread_t thread;
    ret = pthread_mutex_lock(&context.mutex);
    if (!ret) ret = pthread_create(&thread, NULL, pi_worker, &context);
    if (!ret) {
        wait_ready(&context, 1);
        usleep(30000);
        pthread_kill(thread, SIGUSR2);
        usleep(30000);
        ret = pthread_mutex_unlock(&context.mutex);
        if (!ret) ret = pthread_join(thread, NULL);
    }
    check("PI contention and signal restart without SA_RESTART", ret == 0 && context.result == 0);
    for (int timed = 1; timed <= 2; ++timed) {
        context.timed = timed;
        atomic_store(&context.ready, 0);
        ret = pthread_mutex_lock(&context.mutex);
        if (!ret) ret = pthread_create(&thread, NULL, pi_worker, &context);
        if (!ret) ret = pthread_join(thread, NULL);
        pthread_mutex_unlock(&context.mutex);
        check(timed == 1 ? "PI CLOCK_REALTIME timeout" : "PI CLOCK_MONOTONIC timeout",
              ret == 0 && context.result == ETIMEDOUT);
    }
    pthread_mutex_destroy(&context.mutex);
    context = (struct pi_context){0};
    ret = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if (!ret) ret = pthread_mutex_init(&context.mutex, &attr);
    context.owner = 1;
    if (!ret) ret = pthread_create(&thread, NULL, pi_worker, &context);
    if (!ret) {
        wait_ready(&context, 2);
        atomic_store(&context.release, 1);
        ret = pthread_join(thread, NULL);
        if (!ret) ret = pthread_mutex_lock(&context.mutex);
        check("robust PI mutex owner death", ret == EOWNERDEAD);
        if (ret == EOWNERDEAD) {
            check("robust PI consistency recovery", pthread_mutex_consistent(&context.mutex) == 0 &&
                  pthread_mutex_unlock(&context.mutex) == 0);
        }
    } else check("robust PI owner creation", 0);
    pthread_mutex_destroy(&context.mutex);
    pthread_mutexattr_destroy(&attr);
}

__attribute__((target("avx")))
static void avx_signal_handler(int signo, siginfo_t *info, void *opaque)
{
    (void)signo;
    (void)info;
    ucontext_t *context = opaque;
    const unsigned char *state = (const unsigned char *)context->uc_mcontext.fpregs;
    uint32_t magic, size, trailer;
    memcpy(&magic, state + 464, sizeof(magic));
    memcpy(&size, state + 480, sizeof(size));
    if (size >= 832 && size <= 4096) memcpy(&trailer, state + size, sizeof(trailer));
    else trailer = 0;
    signal_xstate_ok = (context->uc_flags & 1) && magic == 0x46505853 &&
        trailer == 0x46505845 && memcmp(state + 576 + 15 * 16, &avx_pattern[2], 16) == 0;
    signal_seen++;
    __asm__ volatile("vpxor %%ymm15, %%ymm15, %%ymm15" : : : "ymm15");
}

__attribute__((target("avx")))
static int avx_yield(uint64_t salt)
{
    uint64_t input[4], output[4];
    for (unsigned i = 0; i < 4; ++i) input[i] = avx_pattern[i] ^ salt;
    for (unsigned i = 0; i < 128; ++i) {
        __asm__ volatile("vmovdqu (%[input]), %%ymm15\n\t"
                         "mov $24, %%eax\n\tsyscall\n\t"
                         "vmovdqu %%ymm15, (%[output])\n\tvzeroupper"
                         : : [input] "r"(input), [output] "r"(output)
                         : "rax", "rcx", "r11", "ymm15", "memory");
        if (memcmp(input, output, sizeof(input))) return 0;
    }
    return 1;
}

static void *avx_worker(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)avx_yield(0xfeed);
}

__attribute__((target("avx")))
static int avx_signal(void)
{
    uint64_t output[4];
    long result;
    long pid = getpid(), tid = syscall(SYS_gettid);
    __asm__ volatile("vmovdqu (%[input]), %%ymm15\n\t"
                     "syscall\n\t"
                     "vmovdqu %%ymm15, (%[output])\n\tvzeroupper"
                     : "=a"(result)
                     : "a"((long)SYS_tgkill), "D"(pid), "S"(tid), "d"((long)SIGUSR1),
                       [input] "r"(avx_pattern), [output] "r"(output)
                     : "rcx", "r11", "ymm15", "memory");
    return result == 0 && memcmp(avx_pattern, output, sizeof(output)) == 0;
}

static void test_avx(void)
{
    unsigned a, b, c, d;
    __cpuid(1, a, b, c, d);
    printf("CPUID AVX=%u OSXSAVE=%u\n", (c >> 28) & 1, (c >> 27) & 1);
    if (!(c & (1u << 28))) {
        puts("SKIP AVX CPU has no AVX feature");
        return;
    }
    check("OSXSAVE available for an AVX CPU", c & (1u << 27));
    if (!(c & (1u << 27))) return;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    check("XCR0 enables x87 SSE and AVX", (a & 7) == 7);
    if ((a & 7) != 7) return;
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, avx_worker, NULL);
    int own_state = avx_yield(0xcafe);
    void *result = NULL;
    if (!ret) ret = pthread_join(thread, &result);
    check("AVX register state across thread scheduling", ret == 0 && own_state && result == (void *)1);
    struct sigaction action = { .sa_sigaction = avx_signal_handler, .sa_flags = SA_SIGINFO };
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, NULL);
    signal_seen = signal_xstate_ok = 0;
    check("AVX register state across signal and sigreturn", avx_signal() && signal_seen == 1);
    check("Linux XSAVE signal-frame metadata and YMM image", signal_xstate_ok);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("GLIBC_RUNTIME_PROBE version=%s\n", gnu_get_libc_version());
    test_cpu_clocks();
    test_rseq();
    test_pi_mutex();
    test_avx();
    printf("GLIBC_RUNTIME_RESULT failures=%u\n", failures);
    return failures ? 1 : 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/* The same native x86-64 probe runs on Linux and ReliefNT. All filters used
 * here target only dedicated test children, their own pipe or identity reads. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "seccomp probe:%d: %s (errno=%d)\n", __LINE__, #cond, errno); \
        _exit(1); \
    } \
} while (0)
#define FAILS(call, code) do { errno = 0; CHECK((call) == -1 && errno == (code)); } while (0)

_Static_assert(sizeof(struct sock_filter) == 8, "classic BPF instruction size");
_Static_assert(sizeof(struct sock_fprog) == 16, "native classic BPF program descriptor size");
_Static_assert(offsetof(struct sock_fprog, filter) == 8, "native classic BPF pointer offset");
_Static_assert(sizeof(struct seccomp_data) == 64, "seccomp data size");
_Static_assert(offsetof(struct seccomp_data, args[5]) == 56, "sixth native argument offset");

static const char *program_path;
static pthread_barrier_t barrier;
static long sibling_tid;

static long seccomp(unsigned int op, unsigned int flags, const void *argument)
{
    return syscall(SYS_seccomp, op, flags, argument);
}

static void nnp(void)
{
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
}

static long install(struct sock_filter *code, unsigned short length, unsigned int flags)
{
    struct sock_fprog program = {length, code};
    return seccomp(SECCOMP_SET_MODE_FILTER, flags, &program);
}

static long rule(int nr, uint32_t response, unsigned int flags, int use_prctl)
{
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)nr, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, response),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {LENGTH(code), code};
    long ret = use_prctl ? prctl(PR_SET_SECCOMP, (long)SECCOMP_MODE_FILTER, &program, 0L, 0L)
                        : seccomp(SECCOMP_SET_MODE_FILTER, flags, &program);
    /* The returned filter must no longer reference user instructions. */
    volatile struct sock_filter *user_code = code;
    user_code[5].k = SECCOMP_RET_ALLOW;
    return ret;
}

static int child_status(void (*test)(void))
{
    pid_t child = fork();
    int status = 0;
    CHECK(child >= 0);
    if (!child) {
        test();
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    return status;
}

static void test_install(void)
{
    uint32_t action = SECCOMP_RET_ALLOW;
    CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0);
    CHECK(prctl(PR_GET_SECCOMP, 7L, 0L, 0L, 0L) == SECCOMP_MODE_DISABLED);
    FAILS(seccomp(SECCOMP_GET_ACTION_AVAIL, 1, &action), EINVAL);
    FAILS(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, NULL), EFAULT);
    action = 0x12340000;
    FAILS(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &action), EOPNOTSUPP);
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, 0, NULL), EFAULT);
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, 1U << 31, NULL), EINVAL);
    struct sock_fprog empty = {0, NULL};
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, 0, &empty), EINVAL);

    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[_LINUX_CAPABILITY_U32S_3] = {{0}};
    CHECK(syscall(SYS_capset, &header, caps) == 0);
    const int inherited_nnp = prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L);
    CHECK(inherited_nnp >= 0);
    if (!inherited_nnp)
        FAILS(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 0), EACCES);
    nnp();

    struct sock_filter malformed[] = {
        BPF_STMT(BPF_LD | BPF_MEM, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    FAILS(install(malformed, LENGTH(malformed), 0), EINVAL);
    struct sock_fprog unreadable = {1, NULL};
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, 0, &unreadable), EINVAL);
    unreadable.filter = (void *)(uintptr_t)1;
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, 0, &unreadable), EFAULT);
    CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_DISABLED);
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 1) == 0);
    CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_FILTER);
    FAILS(syscall(SYS_getppid), EACCES);
    CHECK(syscall(SYS_getpid) > 0);

    FILE *status = fopen("/proc/self/status", "r");
    CHECK(status != NULL);
    char line[256];
    int mode = -1, count = -1;
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "Seccomp:", 8) == 0) mode = atoi(line + 8);
        else if (strncmp(line, "Seccomp_filters:", 16) == 0) count = atoi(line + 16);
    }
    CHECK(fclose(status) == 0);
    CHECK(mode == SECCOMP_MODE_FILTER && count == 1);
}

static void test_filter_stack_and_pipe(void)
{
    int pipefd[2];
    char byte = 'A', readback = 0;
    CHECK(pipe(pipefd) == 0);
    CHECK(write(pipefd[1], &byte, 1) == 1);
    CHECK(read(pipefd[0], &readback, 1) == 1 && readback == 'A');
    CHECK(fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0);
    nnp();

    struct sock_filter pipe_rule[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)pipefd[1], 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EAGAIN),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    CHECK(install(pipe_rule, LENGTH(pipe_rule), 0) == 0);
    FAILS(write(pipefd[1], &byte, 1), EAGAIN);
    FAILS(read(pipefd[0], &readback, 1), EAGAIN);
    CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 0) == 0);
    CHECK(rule(SYS_getppid, SECCOMP_RET_ALLOW, 0, 0) == 0);
    FAILS(syscall(SYS_getppid), EACCES);
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | 65535U, 0, 0) == 0);
    FAILS(syscall(SYS_getppid), 4095);

    CHECK(rule(SYS_fork, SECCOMP_RET_ERRNO | EPERM, 0, 0) == 0);
    FAILS(syscall(SYS_fork), EPERM);
    char *const args[] = {(char *)program_path, (char *)"--fail-if-executed", NULL};
    char *const env[] = {NULL};
    CHECK(rule(SYS_execve, SECCOMP_RET_ERRNO, 0, 0) == 0);
    CHECK(syscall(SYS_execve, program_path, args, env) == 0);
}

static void test_action_fallbacks(void)
{
    uint32_t action = SECCOMP_RET_LOG;
    CHECK(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0);
    FILE *actions = fopen("/proc/sys/kernel/seccomp/actions_avail", "r");
    CHECK(actions != NULL);
    char line[128];
    CHECK(fgets(line, sizeof(line), actions) != NULL && strstr(line, "log allow"));
    CHECK(fclose(actions) == 0);
    nnp();
    CHECK(rule(SYS_getpid, SECCOMP_RET_LOG, 0, 0) == 0);
    CHECK(syscall(SYS_getpid) > 0);
    CHECK(rule(SYS_getppid, SECCOMP_RET_TRACE, 0, 0) == 0);
    FAILS(syscall(SYS_getppid), ENOSYS);
    CHECK(rule(SYS_getppid, SECCOMP_RET_USER_NOTIF, 0, 0) == 0);
    FAILS(syscall(SYS_getppid), ENOSYS);
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, SECCOMP_FILTER_FLAG_LOG, 0) == 0);
    FAILS(syscall(SYS_getppid), EACCES);
}

static void test_path_limit(void)
{
    static struct sock_filter code[BPF_MAXINSNS];
    /* Zero-initialized instructions are LD IMM 0. */
    code[LENGTH(code) - 1] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    nnp();
    for (unsigned int i = 0; i < 7; ++i) CHECK(install(code, LENGTH(code), 0) == 0);
    FAILS(install(code, LENGTH(code), 0), ENOMEM);
    CHECK(syscall(SYS_getpid) > 0);
}

static void test_fork_exec(void)
{
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 0) == 0);
    pid_t grandchild = fork();
    CHECK(grandchild >= 0);
    if (!grandchild) {
        FAILS(syscall(SYS_getppid), EACCES);
        execlp(program_path, program_path, "--after-exec", (char *)NULL);
        CHECK(0 && "exec returned");
    }
    int status;
    CHECK(waitpid(grandchild, &status, 0) == grandchild && status == 0);
}

static void meet(void)
{
    int ret = pthread_barrier_wait(&barrier);
    CHECK(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *thread_check_sync(void *unused)
{
    (void)unused;
    meet();
    meet();
    CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_FILTER);
    CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 1);
    FAILS(syscall(SYS_getppid), EACCES);
    FAILS(syscall(SYS_getuid), EPERM);
    return NULL;
}

static void test_tsync(void)
{
    pthread_t thread;
    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    CHECK(pthread_create(&thread, NULL, thread_check_sync, NULL) == 0);
    meet();
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, SECCOMP_FILTER_FLAG_TSYNC, 0) == 0);
    CHECK(rule(SYS_getuid, SECCOMP_RET_ERRNO | EPERM, 0, 0) == 0);
    /* The sibling is already filtered, but its head is an ancestor. */
    CHECK(rule(SYS_getpid, SECCOMP_RET_ALLOW, SECCOMP_FILTER_FLAG_TSYNC, 0) == 0);
    meet();
    FAILS(syscall(SYS_getppid), EACCES);
    CHECK(pthread_join(thread, NULL) == 0);
}

static void *thread_diverge(void *unused)
{
    (void)unused;
    sibling_tid = syscall(SYS_gettid);
    CHECK(sibling_tid > 0);
    CHECK(rule(SYS_getpid, SECCOMP_RET_ERRNO | EPERM, 0, 0) == 0);
    meet();
    meet();
    FAILS(syscall(SYS_getpid), EPERM);
    CHECK(syscall(SYS_getppid) > 0);
    return NULL;
}

static void test_tsync_divergent(void)
{
    pthread_t thread;
    nnp();
    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    CHECK(pthread_create(&thread, NULL, thread_diverge, NULL) == 0);
    meet();
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, SECCOMP_FILTER_FLAG_TSYNC, 0) == sibling_tid);
    FAILS(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES,
               SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH, 0), ESRCH);
    CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_DISABLED);
    CHECK(syscall(SYS_getppid) > 0);
    meet();
    CHECK(pthread_join(thread, NULL) == 0);
}

static char task_state(long tid)
{
    char name[96], line[256], state = 0;
    CHECK(snprintf(name, sizeof(name), "/proc/self/task/%ld/status", tid) > 0);
    FILE *status = fopen(name, "r");
    CHECK(status);
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "State: %c", &state) == 1) break;
    }
    CHECK(fclose(status) == 0);
    return state;
}

static int parked_read_fd;
static long reader_tid;

static void *parked_read(void *unused)
{
    (void)unused;
    __atomic_store_n(&reader_tid, syscall(SYS_gettid), __ATOMIC_RELEASE);
    char byte = 0;
    CHECK(read(parked_read_fd, &byte, 1) == 1 && byte == 'A');
    FAILS(read(parked_read_fd, &byte, 1), EACCES);
    return NULL;
}

static void test_tsync_parked_read(void)
{
    pthread_t thread;
    int fd[2];
    CHECK(pipe(fd) == 0);
    parked_read_fd = fd[0];
    CHECK(pthread_create(&thread, NULL, parked_read, NULL) == 0);
    long tid;
    while (!(tid = __atomic_load_n(&reader_tid, __ATOMIC_ACQUIRE))) sched_yield();
    while (task_state(tid) != 'S') sched_yield();
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)fd[0], 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    nnp();
    CHECK(install(code, LENGTH(code), SECCOMP_FILTER_FLAG_TSYNC) == 0);
    CHECK(write(fd[1], "AB", 2) == 2);
    CHECK(pthread_join(thread, NULL) == 0);
}

static uintptr_t trap_return_ip;
static volatile sig_atomic_t trap_ok;

static void trap_handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    trap_ok = sig == SIGSYS && info->si_code == SYS_SECCOMP && info->si_errno == 71 &&
              info->si_arch == AUDIT_ARCH_X86_64 && info->si_syscall == SYS_getppid &&
              (uintptr_t)info->si_call_addr == trap_return_ip &&
              (uintptr_t)uc->uc_mcontext.gregs[REG_RIP] == trap_return_ip &&
              uc->uc_mcontext.gregs[REG_RAX] == SYS_getppid;
    uc->uc_mcontext.gregs[REG_RAX] = 1234;
}

static void test_trap(void)
{
    const struct sigaction sa = {.sa_sigaction = trap_handler, .sa_flags = SA_SIGINFO};
    CHECK(sigaction(SIGSYS, &sa, NULL) == 0);
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 0) == 0);
    CHECK(rule(SYS_getppid, SECCOMP_RET_TRAP | 71, 0, 0) == 0);
    long result = SYS_getppid;
    __asm__ volatile("lea 1f(%%rip), %%r10\n\tmov %%r10, %[next]\n\tsyscall\n1:"
                     : "+a"(result), [next] "=m"(trap_return_ip)
                     : : "r10", "rcx", "r11", "cc", "memory");
    CHECK(result == 1234 && trap_ok);
}

static void *thread_forbidden(void *unused)
{
    (void)unused;
    (void)syscall(SYS_getppid);
    CHECK(0 && "forbidden syscall returned");
    return NULL;
}

/* pthread_join relies on libc thread-exit bookkeeping, which KILL_THREAD
 * intentionally skips. Observe the kernel's thread identity instead. */
static int killed_tid, forbidden_returned;

static void *thread_die_alone(void *unused)
{
    (void)unused;
    __atomic_store_n(&killed_tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
    (void)syscall(SYS_getppid);
    __atomic_store_n(&forbidden_returned, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_kill_thread(void)
{
    pthread_t thread;
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_KILL_THREAD, 0, 0) == 0);
    CHECK(pthread_create(&thread, NULL, thread_die_alone, NULL) == 0);
    int tid;
    while (!(tid = __atomic_load_n(&killed_tid, __ATOMIC_ACQUIRE))) sched_yield();
    long pid = syscall(SYS_getpid);
    CHECK(tid > 0 && pid > 0);
    while (syscall(SYS_tgkill, pid, tid, 0) == 0) sched_yield();
    CHECK(errno == ESRCH && !__atomic_load_n(&forbidden_returned, __ATOMIC_ACQUIRE));
}

static void *last_thread(void *unused)
{
    (void)unused;
    long leader = syscall(SYS_getpid);
    while (task_state(leader) != 'Z') sched_yield();
    (void)syscall(SYS_getppid);
    CHECK(0 && "fatal filter returned");
    return NULL;
}

static void test_kill_last_thread(void)
{
    pthread_t thread;
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_KILL_THREAD, 0, 0) == 0);
    CHECK(pthread_create(&thread, NULL, last_thread, NULL) == 0);
    syscall(SYS_exit, 0); /* Only the group leader exits here. */
    CHECK(0 && "raw thread exit returned");
}

static void *group_exit_after_leader(void *unused)
{
    (void)unused;
    long leader = syscall(SYS_getpid);
    while (task_state(leader) != 'Z') sched_yield();
    syscall(SYS_exit_group, 37);
    CHECK(0 && "raw group exit returned");
    return NULL;
}

static void test_leader_kill_then_normal_group_exit(void)
{
    pthread_t thread;
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_KILL_THREAD, 0, 0) == 0);
    CHECK(pthread_create(&thread, NULL, group_exit_after_leader, NULL) == 0);
    (void)syscall(SYS_getppid);
    CHECK(0 && "leader survived KILL_THREAD");
}

static void test_kill_process(void)
{
    pthread_t thread;
    CHECK(signal(SIGSYS, SIG_IGN) != SIG_ERR);
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_KILL_PROCESS, 0, 0) == 0);
    CHECK(pthread_create(&thread, NULL, thread_forbidden, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(0 && "thread group survived the terminal action");
}

static void test_ignored_trap(void)
{
    CHECK(signal(SIGSYS, SIG_IGN) != SIG_ERR);
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_TRAP, 0, 0) == 0);
    (void)syscall(SYS_getppid);
    CHECK(0 && "synchronous SIGSYS was ignored");
}

/* ReliefNT's legacy int 0x80 front door decodes native x86-64 registers.
 * This is not Linux's IA32 personality and is tested only on ReliefNT. */
static void test_reliefnt_extensions(void)
{
    uint32_t action = SECCOMP_RET_USER_NOTIF;
    FAILS(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &action), EOPNOTSUPP);
    action = SECCOMP_RET_TRACE;
    FAILS(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &action), EOPNOTSUPP);
    FAILS(seccomp(SECCOMP_GET_NOTIF_SIZES, 0, NULL), EINVAL);
    FAILS(seccomp(SECCOMP_SET_MODE_STRICT, 0, NULL), EINVAL);
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, NULL), EINVAL);
    FAILS(seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW, NULL), EINVAL);
    nnp();
    CHECK(rule(SYS_getppid, SECCOMP_RET_ERRNO | EACCES, 0, 0) == 0);
    long result = SYS_getppid;
    __asm__ volatile("int $0x80" : "+a"(result) : : "memory", "cc");
    CHECK(result == -EACCES);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--after-exec")) {
        CHECK(prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) == SECCOMP_MODE_FILTER);
        CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 1);
        FAILS(syscall(SYS_getppid), EACCES);
        return 0;
    }
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--reliefnt")));
    program_path = argv[0];
    CHECK(setrlimit(RLIMIT_CORE, &(struct rlimit){0, 0}) == 0);
    struct { const char *name; void (*test)(void); int status; } cases[] = {
        {"install, permissions and status", test_install, 0},
        {"stacked actions, pipe and denied fork/exec", test_filter_stack_and_pipe, 0},
        {"action probing, logging and notifier/tracer fallback", test_action_fallbacks, 0},
        {"maximum filter path is bounded", test_path_limit, 0},
        {"fork and exec inheritance", test_fork_exec, 0},
        {"TSYNC across threads", test_tsync, 0},
        {"atomic TSYNC failure and ESRCH", test_tsync_divergent, 0},
        {"TSYNC keeps an already parked read admitted", test_tsync_parked_read, 0},
        {"SIGSYS siginfo and register context", test_trap, 0},
        {"KILL_THREAD only terminates its thread", test_kill_thread, 0},
        {"last KILL_THREAD reports SIGSYS to the parent", test_kill_last_thread, -SIGSYS},
        {"explicit exit_group sets the surviving group's status",
         test_leader_kill_then_normal_group_exit, 37},
        {"KILL_PROCESS terminates the group", test_kill_process, -SIGSYS},
        {"TRAP cannot be ignored", test_ignored_trap, -SIGSYS},
    };
    int failures = 0;
    for (size_t i = 0; i < LENGTH(cases); ++i) {
        int status = child_status(cases[i].test);
        const int expected = cases[i].status;
        if (expected < 0 ? !WIFSIGNALED(status) || WTERMSIG(status) != -expected :
                           !WIFEXITED(status) || WEXITSTATUS(status) != expected) {
            fprintf(stderr, "FAIL seccomp %s (wait status=%#x)\n", cases[i].name, status);
            ++failures;
            continue;
        }
        printf("PASS seccomp %s\n", cases[i].name);
        fflush(stdout);
    }
    if (argc == 2) {
        CHECK(child_status(test_reliefnt_extensions) == 0);
        puts("PASS seccomp unsupported features and legacy native entry");
    }
    if (failures) return 1;
    puts("SECCOMP_ABI_PASS");
    return 0;
}

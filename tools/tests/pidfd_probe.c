/* SPDX-License-Identifier: Apache-2.0 */
/* Native x86-64 ABI probe shared by ReliefNT guests and Linux references.
 * Signals are sent only to this program's own threads and test children. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Native Linux values even if the probe's libc predates these names. */
#define PIDFD_WAIT 3
#define PIDFD_THREAD_FLAG O_EXCL
#define PIDFD_SCOPE_THREAD (1U << 0)
#define PIDFD_SCOPE_PROCESS (1U << 1)
#define WAIT_NO_SIBLING 0x20000000

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "pidfd probe:%d: %s (errno=%d)\n", __LINE__, #cond, errno); \
        _exit(1); \
    } \
} while (0)
#define FAILS(call, code) do { errno = 0; CHECK((call) == -1 && errno == (code)); } while (0)

static const char *program_path;
static int cases;

static int open_pid(pid_t pid, unsigned flags)
{
    return (int)syscall(SYS_pidfd_open, pid, flags);
}

static int send_pid(int fd, int sig, const siginfo_t *info, unsigned flags)
{
    return (int)syscall(SYS_pidfd_send_signal, fd, sig, info, flags);
}

static int wait_pid(int fd, siginfo_t *info, int options)
{
    return (int)syscall(SYS_waitid, PIDFD_WAIT, fd, info, options, NULL);
}

static void pass(const char *name)
{
    ++cases;
    printf("PASS %s\n", name);
}

static void fdinfo_pid(int fd, pid_t target)
{
    char path[64], expected[36], data[512];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    snprintf(expected, sizeof(expected), "\nPid:\t%d\n", target);
    int input = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(input >= 0);
    ssize_t length = read(input, data, sizeof(data) - 1);
    CHECK(length > 0);
    data[length] = 0;
    CHECK(close(input) == 0);
    CHECK(strstr(data, expected));
}

static short readiness(int fd, short events, int timeout, int count)
{
    struct pollfd p = {fd, events, 0};
    CHECK(poll(&p, 1, timeout) == count);
    return p.revents;
}

static void check_basic(void)
{
    int fd = open_pid(getpid(), 0);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(fd, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == O_RDWR);
    CHECK(send_pid(fd, 0, NULL, 0) == 0);
    CHECK(readiness(fd, POLLIN | POLLOUT, 0, 0) == 0);
    fdinfo_pid(fd, getpid());
    char byte = 0;
    struct iovec v = {&byte, 1};
    FAILS(read(fd, &byte, 1), EINVAL);
    FAILS(write(fd, &byte, 1), EINVAL);
    FAILS(readv(fd, &v, 1), EINVAL);
    FAILS(writev(fd, &v, 1), EINVAL);
    FAILS(lseek(fd, 0, SEEK_SET), ESPIPE);
    FAILS(open_pid(0, 0), EINVAL);
    FAILS(open_pid(-1, 0), EINVAL);
    FAILS(open_pid(getpid(), O_CLOEXEC), EINVAL);
    FAILS(open_pid(INT32_MAX, 0), ESRCH);
    FAILS(send_pid(-1, 0, NULL, 0), EBADF);
    FAILS(send_pid(fd, -1, NULL, 0), EINVAL);
    FAILS(send_pid(fd, 65, NULL, 0), EINVAL);
    FAILS(send_pid(fd, 0, NULL, 1U << 30), EINVAL);
    FAILS(send_pid(fd, 0, NULL, PIDFD_SCOPE_THREAD | PIDFD_SCOPE_PROCESS), EINVAL);
    FAILS(send_pid(fd, 0, (const siginfo_t *)(uintptr_t)1, 0), EFAULT);
    siginfo_t info = {.si_signo = SIGUSR1, .si_code = SI_QUEUE};
    FAILS(send_pid(fd, 0, &info, 0), EINVAL);
    FAILS(wait_pid(-1, &info, WEXITED), EINVAL);
    FAILS(wait_pid(fd, &info, WNOHANG), EINVAL);
    FAILS(wait_pid(fd, &info, WEXITED | WNOHANG), ECHILD);
    int ordinary = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(ordinary >= 0);
    FAILS(send_pid(ordinary, 0, NULL, 0), EBADF);
    FAILS(wait_pid(ordinary, &info, WEXITED | WNOHANG), EBADF);
    CHECK(close(ordinary) == 0);
    int copy = dup(fd);
    CHECK(copy >= 0 && fcntl(copy, F_GETFD) == 0);
    CHECK(close(fd) == 0);
    CHECK(send_pid(copy, 0, NULL, 0) == 0);
    CHECK(close(copy) == 0);
    pass("descriptor flags, readiness, errors and dup ownership");
}

static void check_child_lifetime(void)
{
    int control[2];
    CHECK(pipe(control) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        char byte;
        close(control[1]);
        CHECK(read(control[0], &byte, 1) == 1);
        struct timespec begin, now;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
        do {
            for (volatile unsigned work = 0; work < 200000U; ++work) { }
            CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        } while ((now.tv_sec - begin.tv_sec) * 1000000000LL + now.tv_nsec - begin.tv_nsec < 100000000);
        _exit(37);
    }
    CHECK(close(control[0]) == 0);
    int fd = open_pid(child, O_NONBLOCK), alias = open_pid(child, 0);
    CHECK(fd >= 0 && alias >= 0);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(alias, F_GETFL) & O_NONBLOCK) == 0);
    siginfo_t info;
    FAILS(wait_pid(fd, &info, WEXITED), EAGAIN);
    struct rusage usage, untouched;
    memset(&usage, 0x5a, sizeof(usage));
    memcpy(&untouched, &usage, sizeof(usage));
    CHECK(syscall(SYS_waitid, PIDFD_WAIT, fd, &info, WEXITED | WNOHANG, &usage) == 0);
    CHECK(memcmp(&usage, &untouched, sizeof(usage)) == 0);
    memset(&info, 0x55, sizeof(info));
    CHECK(wait_pid(fd, &info, WEXITED | WNOHANG) == 0 && info.si_pid == 0 && info.si_signo == 0);
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 123};
    CHECK(epfd >= 0 && epoll_ctl(epfd, EPOLL_CTL_ADD, alias, &event) == 0);
    CHECK(epoll_wait(epfd, &event, 1, 0) == 0);
    CHECK(write(control[1], "x", 1) == 1);
    CHECK(close(control[1]) == 0);
    CHECK(epoll_wait(epfd, &event, 1, 4000) == 1);
    CHECK(event.data.u64 == 123 && (event.events & (EPOLLIN | EPOLLHUP)) == EPOLLIN);
    CHECK(epoll_wait(epfd, &event, 1, 0) == 0);
    CHECK(readiness(fd, POLLIN | POLLOUT, 0, 1) == POLLIN);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval instant = {0, 0};
    CHECK(select(fd + 1, &set, NULL, NULL, &instant) == 1 && FD_ISSET(fd, &set));
    /* Exited but not reaped; a newly opened fd still refers to this zombie. */
    int zombie = open_pid(child, 0);
    CHECK(zombie >= 0 && readiness(zombie, POLLIN, 0, 1) == POLLIN);
    CHECK(send_pid(fd, 0, NULL, 0) == 0);
    FAILS(syscall(SYS_waitid, PIDFD_WAIT, alias, &info, WEXITED | WNOWAIT,
                  (void *)(uintptr_t)1), EFAULT);
    CHECK(syscall(SYS_waitid, PIDFD_WAIT, alias, &info, WEXITED | WNOWAIT, &usage) == 0);
    CHECK(memcmp(&usage, &untouched, sizeof(usage)) != 0);
    CHECK(usage.ru_utime.tv_sec >= 0 && usage.ru_utime.tv_usec >= 0 && usage.ru_utime.tv_usec < 1000000);
    CHECK(usage.ru_stime.tv_sec >= 0 && usage.ru_stime.tv_usec >= 0 && usage.ru_stime.tv_usec < 1000000);
    CHECK(usage.ru_utime.tv_sec || usage.ru_utime.tv_usec || usage.ru_stime.tv_sec || usage.ru_stime.tv_usec);
    CHECK(info.si_pid == child && info.si_signo == SIGCHLD && info.si_code == CLD_EXITED && info.si_status == 37);
    FAILS(syscall(SYS_waitid, PIDFD_WAIT, alias, (void *)(uintptr_t)1, WEXITED,
                  NULL), EFAULT);
    CHECK((readiness(fd, POLLIN, 0, 1) & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP));
    CHECK(readiness(zombie, 0, 0, 1) == POLLHUP);
    CHECK(epoll_wait(epfd, &event, 1, 0) == 1 && (event.events & EPOLLHUP));
    CHECK(epoll_wait(epfd, &event, 1, 0) == 0);
    fdinfo_pid(fd, -1);
    FAILS(send_pid(fd, 0, NULL, 0), ESRCH);
    FAILS(send_pid(zombie, SIGTERM, NULL, 0), ESRCH);
    FAILS(open_pid(child, 0), ESRCH);
    FAILS(wait_pid(fd, &info, WEXITED | WNOHANG), ECHILD);
    CHECK(close(zombie) == 0 && close(fd) == 0 && close(alias) == 0 && close(epfd) == 0);
    pass("poll/select/epoll lifetime, zombie reap and stale handles");
}

static void check_signal_and_blocking_wait(void)
{
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        sigset_t mask;
        siginfo_t info;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
        CHECK(close(ready[0]) == 0 && write(ready[1], "x", 1) == 1);
        CHECK(sigwaitinfo(&mask, &info) == SIGUSR1);
        CHECK(info.si_code == SI_QUEUE && info.si_value.sival_int == 42);
        _exit(12);
    }
    char byte;
    CHECK(close(ready[1]) == 0 && read(ready[0], &byte, 1) == 1 && close(ready[0]) == 0);
    int fd = open_pid(child, 0);
    CHECK(fd >= 0);
    siginfo_t info = {.si_signo = SIGUSR1, .si_code = SI_USER, .si_pid = getpid(), .si_uid = getuid()};
    FAILS(send_pid(fd, SIGUSR1, &info, 0), EPERM);
    info.si_code = SI_QUEUE;
    info.si_value.sival_int = 42;
    CHECK(send_pid(fd, SIGUSR1, &info, PIDFD_SCOPE_PROCESS) == 0);
    CHECK(wait_pid(fd, &info, WEXITED) == 0 && info.si_pid == child && info.si_status == 12);
    CHECK(close(fd) == 0);
    pass("queued siginfo, permission on sender fields and blocking waitid");
}

static int thread_ready[2], thread_exit[2];

static void *pidfd_thread(void *unused)
{
    (void)unused;
    sigset_t mask;
    siginfo_t info;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    pid_t tid = (pid_t)syscall(SYS_gettid);
    CHECK(write(thread_ready[1], &tid, sizeof(tid)) == (ssize_t)sizeof(tid));
    CHECK(sigwaitinfo(&mask, &info) == SIGUSR2);
    CHECK(write(thread_exit[1], "x", 1) == 1);
    return NULL;
}

static void check_thread(void)
{
    pthread_t thread;
    pid_t tid;
    sigset_t group_mask, saved_mask;
    sigemptyset(&group_mask);
    sigaddset(&group_mask, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &group_mask, &saved_mask) == 0);
    CHECK(pipe(thread_ready) == 0 && pipe(thread_exit) == 0);
    CHECK(pthread_create(&thread, NULL, pidfd_thread, NULL) == 0);
    CHECK(read(thread_ready[0], &tid, sizeof(tid)) == (ssize_t)sizeof(tid));
    /* Linux v6.12 used EINVAL; newer Linux uses ENOENT for a live nonleader. */
    CHECK(open_pid(tid, 0) == -1 && (errno == EINVAL || errno == ENOENT));
    int fd = open_pid(tid, PIDFD_THREAD_FLAG);
    CHECK(fd >= 0 && readiness(fd, POLLIN, 0, 0) == 0);
    siginfo_t info;
    FAILS(wait_pid(fd, &info, WEXITED | WNOHANG), ECHILD);
    CHECK(send_pid(fd, 0, NULL, 0) == 0);
    CHECK(send_pid(fd, 0, NULL, PIDFD_SCOPE_THREAD) == 0);
    CHECK(send_pid(fd, 0, NULL, PIDFD_SCOPE_PROCESS) == 0);
    CHECK(send_pid(fd, SIGUSR1, NULL, PIDFD_SCOPE_PROCESS) == 0);
    CHECK(sigwaitinfo(&group_mask, &info) == SIGUSR1);
    CHECK(info.si_code == SI_USER && info.si_pid == getpid());
    CHECK(send_pid(fd, SIGUSR2, NULL, 0) == 0);
    char byte;
    CHECK(read(thread_exit[0], &byte, 1) == 1);
    CHECK(readiness(fd, POLLIN, 4000, 1) & POLLIN);
    CHECK(pthread_join(thread, NULL) == 0);
    FAILS(send_pid(fd, 0, NULL, 0), ESRCH);
    FAILS(send_pid(fd, 0, NULL, PIDFD_SCOPE_THREAD), ESRCH);
    FAILS(send_pid(fd, 0, NULL, PIDFD_SCOPE_PROCESS), ESRCH);
    FAILS(send_pid(fd, SIGUSR1, NULL, PIDFD_SCOPE_PROCESS), ESRCH);
    CHECK(pthread_sigmask(SIG_SETMASK, &saved_mask, NULL) == 0);
    CHECK(close(fd) == 0);
    CHECK(close(thread_ready[0]) == 0 && close(thread_ready[1]) == 0);
    CHECK(close(thread_exit[0]) == 0 && close(thread_exit[1]) == 0);
    pass("thread/group signal scopes, retired thread rejection and exit wake");
}

struct wait_owner {
    int ready[2], finish[2], child_exit[2];
};

static void *wait_owner_thread(void *arg)
{
    const struct wait_owner *owner = arg;
    char byte;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(read(owner->child_exit[0], &byte, 1) == 1);
        _exit(23);
    }
    int fd = open_pid(child, 0);
    CHECK(fd >= 0);
    CHECK(wait_pid(fd, NULL, WEXITED | WNOHANG | WAIT_NO_SIBLING) == 0);
    int handles[] = {fd, open_pid((pid_t)syscall(SYS_gettid), PIDFD_THREAD_FLAG)};
    CHECK(handles[1] >= 0 && write(owner->ready[1], handles, sizeof(handles)) == (ssize_t)sizeof(handles));
    CHECK(read(owner->finish[0], &byte, 1) == 1);
    return NULL;
}

static void check_wait_thread_owner(void)
{
    struct wait_owner owner;
    pthread_t thread;
    int handles[2];
    siginfo_t info;
    CHECK(pipe(owner.ready) == 0 && pipe(owner.finish) == 0 && pipe(owner.child_exit) == 0);
    CHECK(pthread_create(&thread, NULL, wait_owner_thread, &owner) == 0);
    CHECK(read(owner.ready[0], handles, sizeof(handles)) == (ssize_t)sizeof(handles));
    int fd = handles[0];
    FAILS(wait_pid(fd, &info, WEXITED | WNOHANG | WAIT_NO_SIBLING), ECHILD);
    CHECK(wait_pid(fd, &info, WEXITED | WNOHANG) == 0 && info.si_pid == 0);
    CHECK(write(owner.finish[1], "x", 1) == 1);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(readiness(handles[1], POLLIN, 4000, 1) & POLLIN);
    CHECK(close(handles[1]) == 0);
    CHECK(write(owner.child_exit[1], "x", 1) == 1);
    CHECK(wait_pid(fd, &info, WEXITED | WAIT_NO_SIBLING) == 0 && info.si_status == 23);
    CHECK(close(fd) == 0);
    for (int i = 0; i < 2; ++i) {
        CHECK(close(owner.ready[i]) == 0 && close(owner.finish[i]) == 0);
        CHECK(close(owner.child_exit[i]) == 0);
    }
    pass("waitid __WNOTHREAD and reparenting to a surviving sibling");
}

static void check_exec(void)
{
    int fd = open_pid(getpid(), 0);
    int copy = dup(fd);
    CHECK(fd >= 0 && copy >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char closed[24], inherited[24];
        snprintf(closed, sizeof(closed), "%d", fd);
        snprintf(inherited, sizeof(inherited), "%d", copy);
        execl(program_path, program_path, "--exec-child", closed, inherited, (char *)NULL);
        CHECK(0 && "exec pidfd probe");
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(fd) == 0 && close(copy) == 0);
    pass("close-on-exec and inherited open-file description");
}

static void check_credential_change(void)
{
    if (geteuid() != 0) {
        puts("SKIP credential-change check (requires root in the test guest)");
        return;
    }
    pid_t parent = getpid();
    int fd = open_pid(parent, 0);
    CHECK(fd >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setgid(65534) == 0 && setuid(65534) == 0);
        FAILS(send_pid(fd, 0, NULL, 0), EPERM);
        int fresh = open_pid(parent, 0);
        CHECK(fresh >= 0);
        FAILS(send_pid(fresh, SIGTERM, NULL, 0), EPERM);
        CHECK(close(fresh) == 0 && close(fd) == 0);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(fd) == 0);
    pass("credentials rechecked after dropping UID on inherited pidfd");
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--exec-child") == 0) {
        FAILS(fcntl(atoi(argv[2]), F_GETFD), EBADF);
        CHECK(send_pid(atoi(argv[3]), 0, NULL, 0) == 0);
        return 0;
    }
    program_path = argv[0];
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(15);
    check_basic();
    check_child_lifetime();
    check_signal_and_blocking_wait();
    check_thread();
    check_wait_thread_owner();
    check_exec();
    check_credential_change();
    printf("PIDFD_ABI_PASS:%d\n", cases);
    return 0;
}

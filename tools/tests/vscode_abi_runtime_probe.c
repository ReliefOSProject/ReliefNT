#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
        exit(1); \
    } \
} while (0)

#define MIB (1024u * 1024u)

/* Compare the legacy and modern syscall entry points used by both libcs. */
static struct rlimit data_limit(void)
{
    struct rlimit legacy, modern;
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_DATA, NULL, &modern) == 0);
    CHECK(syscall(SYS_getrlimit, RLIMIT_DATA, &legacy) == 0);
    CHECK(legacy.rlim_cur == modern.rlim_cur && legacy.rlim_max == modern.rlim_max);
    return modern;
}

/* Restore the soft limit without modifying the caller's hard limit. */
static void data_set(struct rlimit limit)
{
    CHECK(syscall(SYS_setrlimit, RLIMIT_DATA, &limit) == 0);
}

/* Exercise a finite data limit against mapping changes and byte-granular brk. */
static void test_data(void)
{
    struct rlimit original = data_limit();
    struct rlimit finite = {16 * MIB, original.rlim_max};
    CHECK(original.rlim_max >= finite.rlim_cur);
    struct rlimit previous;
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_DATA, &finite, &previous) == 0);
    CHECK(previous.rlim_cur == original.rlim_cur && previous.rlim_max == original.rlim_max);
    CHECK(data_limit().rlim_cur == finite.rlim_cur);

    const size_t length = 10 * MIB;
    void *private_map = mmap(NULL, length, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(private_map != MAP_FAILED);
    CHECK(mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0) == MAP_FAILED && errno == ENOMEM);
    void *reserved = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(reserved != MAP_FAILED);
    CHECK(mprotect(reserved, length, PROT_WRITE) == -1 && errno == ENOMEM);
    void *moved = mremap(private_map, length, length, MREMAP_MAYMOVE | MREMAP_FIXED, reserved);
    CHECK(moved == reserved);
    CHECK(mremap(moved, length, 17 * MIB, MREMAP_MAYMOVE) == MAP_FAILED && errno == ENOMEM);
    CHECK(mmap(moved, length, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == moved);
    CHECK(munmap(moved, length) == 0);
    void *shared = mmap(NULL, 17 * MIB, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    CHECK(munmap(shared, 17 * MIB) == 0);

    uintptr_t initial = (uintptr_t)syscall(SYS_brk, 0);
    uintptr_t grown = initial + 1;
    CHECK((uintptr_t)syscall(SYS_brk, grown) == grown);
    CHECK((uintptr_t)syscall(SYS_brk, initial + 17 * MIB) == grown);
    data_set((struct rlimit){1, original.rlim_max});
    CHECK((uintptr_t)syscall(SYS_brk, grown + 1) == grown);
    data_set(original);
    CHECK((uintptr_t)syscall(SYS_brk, initial) == initial);
    puts("RLIMIT_DATA PASS");
}

extern char probe_int3_resume[];
static volatile sig_atomic_t breakpoint_seen;

/* The x86 breakpoint is delivered after its one-byte instruction. */
static void breakpoint_handler(int signal, siginfo_t *info, void *context)
{
    const ucontext_t *frame = context;
    breakpoint_seen = signal == SIGTRAP && info->si_code == SI_KERNEL &&
        (uintptr_t)frame->uc_mcontext.gregs[REG_RIP] == (uintptr_t)probe_int3_resume;
}

/* Check the Linux signal ABI with a handler and again with default disposition. */
static void test_breakpoint(void)
{
    struct sigaction action = {.sa_sigaction = breakpoint_handler, .sa_flags = SA_SIGINFO};
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGTRAP, &action, NULL) == 0);
    __asm__ volatile("int3\nprobe_int3_resume:" ::: "memory");
    CHECK(breakpoint_seen);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        action.sa_handler = SIG_DFL;
        action.sa_flags = 0;
        if (sigaction(SIGTRAP, &action, NULL) != 0 ||
            setrlimit(RLIMIT_CORE, &(struct rlimit){0, 0}) != 0)
            _exit(2);
        __asm__ volatile("int3" ::: "memory");
        _exit(3);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP);
    puts("INT3 SIGTRAP PASS");
}

/* Match libuv's drain buffer and check that only one eventfd word is used. */
static void test_eventfd_buffer(void)
{
    alarm(10);
    int fd = eventfd(7, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(fd >= 0);
    uint64_t buffer[128];
    memset(buffer, 0x5a, sizeof(buffer));
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(uint64_t));
    CHECK(buffer[0] == 7);
    for (unsigned i = 1; i < sizeof(buffer) / sizeof(buffer[0]); ++i)
        CHECK(buffer[i] == UINT64_C(0x5a5a5a5a5a5a5a5a));
    CHECK(read(fd, buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
    buffer[0] = 3;
    CHECK(write(fd, buffer, sizeof(buffer)) == -1 && errno == EINVAL);
    CHECK(write(fd, buffer, sizeof(uint64_t)) == sizeof(uint64_t));
    CHECK(read(fd, buffer, 7) == -1 && errno == EINVAL);
    CHECK(read(fd, buffer, 9) == sizeof(uint64_t) && buffer[0] == 3);
    CHECK(close(fd) == 0);
    puts("EVENTFD oversized buffer PASS");
    CHECK(fflush(stdout) == 0);

    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    CHECK(fd >= 0);
    struct itimerspec timer = {.it_value = {.tv_nsec = 1}};
    CHECK(timerfd_settime(fd, 0, &timer, NULL) == 0);
    CHECK(nanosleep(&(struct timespec){.tv_nsec = 100000000}, NULL) == 0);
    CHECK(read(fd, buffer, 7) == -1 && errno == EINVAL);
    CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(uint64_t) && buffer[0] == 1);
    CHECK(read(fd, buffer, sizeof(buffer)) == -1 && errno == EAGAIN);
    CHECK(close(fd) == 0);
    alarm(0);
    puts("TIMERFD oversized buffer and nonblocking drain PASS");
}

/* Linux's mmap descriptor lookup uses the low 32 bits of the fd register. */
static void test_mmap_descriptor(void)
{
    int fd = (int)syscall(SYS_memfd_create, "vscode-map-fd", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, 256 * 1024) == 0);
    uint64_t raw_fd = UINT64_C(0x2d00000000) | (uint32_t)fd;
    unsigned char *mapping = (void *)syscall(SYS_mmap, NULL, 256 * 1024,
        PROT_READ | PROT_WRITE, MAP_SHARED, raw_fd, 0);
    CHECK(mapping != MAP_FAILED);
    mapping[0] = 0x5a;
    CHECK(munmap(mapping, 256 * 1024) == 0);
    unsigned char byte;
    CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 0x5a);
    CHECK((void *)syscall(SYS_mmap, NULL, 4096, PROT_READ, MAP_SHARED,
        UINT64_C(0x2dffffffff), 0) == MAP_FAILED && errno == EBADF);
    CHECK(close(fd) == 0);
    puts("MMAP low32 descriptor PASS");
}

/* Electron waits on an epoll containing libuv's own epoll descriptor. */
static void test_nested_epoll(void)
{
    alarm(10);
    int source = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int inner = epoll_create1(EPOLL_CLOEXEC);
    int outer = epoll_create1(EPOLL_CLOEXEC);
    CHECK(source >= 0 && inner >= 0 && outer >= 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 0x11};
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, source, &interest) == 0);
    interest.data.u64 = 0x22;
    CHECK(epoll_ctl(outer, EPOLL_CTL_ADD, inner, &interest) == 0);
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, outer, &interest) == -1 && errno == ELOOP);
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, inner, &interest) == -1 && errno == EINVAL);
    struct epoll_event ready;
    CHECK(epoll_wait(outer, &ready, 1, 0) == 0);
    uint64_t word = 1;
    CHECK(write(source, &word, sizeof(word)) == sizeof(word));
    struct pollfd pollfd = {.fd = outer, .events = POLLIN};
    CHECK(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLIN));
    CHECK(epoll_wait(outer, &ready, 1, 200) == 1 && ready.data.u64 == 0x22);
    CHECK(epoll_wait(inner, &ready, 1, 0) == 1 && ready.data.u64 == 0x11);
    CHECK(read(source, &word, sizeof(word)) == sizeof(word));
    CHECK(epoll_wait(outer, &ready, 1, 0) == 0);
    CHECK(epoll_ctl(outer, EPOLL_CTL_DEL, inner, NULL) == 0);
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, outer, &interest) == 0);
    CHECK(close(outer) == 0);
    CHECK(epoll_wait(inner, &ready, 1, 0) == 0);
    CHECK(close(inner) == 0 && close(source) == 0);
    alarm(0);
    puts("EPOLL nesting, readiness, cycles and close PASS");
}

/* A proc FD link must return the live object's metadata, not stat its label. */
static void check_proc_fd_stat(int proc_fds, int fd, const char *kind)
{
    char name[32];
    struct stat direct, followed, link;
    struct statx extended;
    printf("proc fd stat: %s (%d)\n", kind, fd);
    CHECK(fd >= 0 && snprintf(name, sizeof(name), "%d", fd) > 0);
    CHECK(fstat(fd, &direct) == 0);
    CHECK(fstatat(proc_fds, name, &link, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(link.st_mode));
    CHECK(fstatat(proc_fds, name, &followed, 0) == 0);
    CHECK(followed.st_dev == direct.st_dev && followed.st_ino == direct.st_ino &&
          followed.st_mode == direct.st_mode && followed.st_rdev == direct.st_rdev);
    CHECK(statx(proc_fds, name, 0, STATX_BASIC_STATS, &extended) == 0);
    CHECK(extended.stx_ino == direct.st_ino && extended.stx_mode == direct.st_mode);
    if (!S_ISDIR(direct.st_mode)) {
        CHECK(snprintf(name, sizeof(name), "%d/.", fd) > 0);
        CHECK(fstatat(proc_fds, name, &followed, 0) == -1 && errno == ENOTDIR);
    }
}

static void test_proc_fd_stat(void)
{
    int proc = open("/proc", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    CHECK(proc >= 0);
    int proc_fds = openat(proc, "self/fd", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    CHECK(proc_fds >= 0);
    int efd = eventfd(0, EFD_CLOEXEC), ep = epoll_create1(EPOLL_CLOEXEC);
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    int pipe_ends[2], sockets[2];
    CHECK(pipe2(pipe_ends, O_CLOEXEC) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    check_proc_fd_stat(proc_fds, efd, "eventfd");
    check_proc_fd_stat(proc_fds, ep, "epoll");
    check_proc_fd_stat(proc_fds, timer, "timerfd");
    check_proc_fd_stat(proc_fds, pipe_ends[0], "anonymous pipe");
    check_proc_fd_stat(proc_fds, sockets[0], "UNIX socket");
    check_proc_fd_stat(proc_fds, proc, "directory");
    CHECK(close(efd) == 0);
    char name[32];
    struct stat st;
    CHECK(snprintf(name, sizeof(name), "%d", efd) > 0);
    CHECK(fstatat(proc_fds, name, &st, 0) == -1 && errno == ENOENT);
    CHECK(close(ep) == 0 && close(timer) == 0 && close(pipe_ends[0]) == 0 &&
          close(pipe_ends[1]) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0 &&
          close(proc_fds) == 0 && close(proc) == 0);
    puts("PROC FD follow and descriptor metadata PASS");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "data") == 0) test_data();
    else if (strcmp(argv[1], "breakpoint") == 0) test_breakpoint();
    else if (strcmp(argv[1], "eventfd") == 0) test_eventfd_buffer();
    else if (strcmp(argv[1], "mmapfd") == 0) test_mmap_descriptor();
    else if (strcmp(argv[1], "epoll") == 0) test_nested_epoll();
    else if (strcmp(argv[1], "procfd") == 0) test_proc_fd_stat();
    else return 2;
    return 0;
}

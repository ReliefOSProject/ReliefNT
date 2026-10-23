#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <termios.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL resources line=%d errno=%d: %s\n", __LINE__, errno, #expr); return 1; \
} } while (0)

static int test_pipes_select(void)
{
    int pairs[300][2];
    for (unsigned i = 0; i < 300; ++i) CHECK(pipe2(pairs[i], O_NONBLOCK | O_CLOEXEC) == 0);
    CHECK(fcntl(pairs[0][0], F_GETPIPE_SZ) >= 4096);
    CHECK(fcntl(pairs[0][1], F_SETPIPE_SZ, 5000) == 8192);
    char bytes[7000], result[7000];
    memset(bytes, 'p', sizeof(bytes));
    CHECK(write(pairs[0][1], bytes, sizeof(bytes)) == sizeof(bytes));
    errno = 0;
    CHECK(fcntl(pairs[0][0], F_SETPIPE_SZ, 4096) == -1 && errno == EBUSY);
    CHECK(read(pairs[0][0], result, sizeof(result)) == sizeof(result));
    CHECK(!memcmp(bytes, result, sizeof(bytes)));
    int high = fcntl(pairs[299][0], F_DUPFD_CLOEXEC, 1100);
    CHECK(high >= 1100);
    CHECK(write(pairs[299][1], "x", 1) == 1);
    size_t words = ((size_t)high + 64) / 64;
    uint64_t *bits = calloc(words, sizeof(*bits));
    CHECK(bits);
    bits[(unsigned)high / 64] = UINT64_C(1) << (high & 63);
    struct timeval timeout = {0, 0};
    CHECK(syscall(SYS_select, high + 1, bits, NULL, NULL, &timeout) == 1);
    CHECK(bits[(unsigned)high / 64] == (UINT64_C(1) << (high & 63)));
    free(bits);
    CHECK(close(high) == 0);
    for (unsigned i = 0; i < 300; ++i) {
        CHECK(close(pairs[i][0]) == 0); CHECK(close(pairs[i][1]) == 0);
    }
    puts("PASS resources: 300 pipes, pipe sizing and select fd > 1023");
    return 0;
}

static int test_epoll(void)
{
    int poller = epoll_create1(EPOLL_CLOEXEC), fds[256];
    CHECK(poller >= 0);
    struct pollfd descriptor = {.fd = poller, .events = POLLIN};
    CHECK(poll(&descriptor, 1, 0) == 0 && !descriptor.revents);
    for (unsigned i = 0; i < 256; ++i) {
        fds[i] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(fds[i] >= 0);
        struct epoll_event event = {.events = EPOLLIN, .data.u64 = i};
        CHECK(epoll_ctl(poller, EPOLL_CTL_ADD, fds[i], &event) == 0);
    }
    uint64_t one = 1;
    CHECK(write(fds[255], &one, sizeof(one)) == sizeof(one));
    CHECK(poll(&descriptor, 1, 0) == 1 && descriptor.revents == POLLIN);
    struct epoll_event ready;
    CHECK(epoll_wait(poller, &ready, 1, 0) == 1 && ready.data.u64 == 255 && (ready.events & EPOLLIN));
    CHECK(read(fds[255], &one, sizeof(one)) == sizeof(one) && one == 1);
    CHECK(poll(&descriptor, 1, 0) == 0 && !descriptor.revents);
    struct epoll_event oneshot = {
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT, .data.u64 = 0x30000};
    CHECK(!epoll_ctl(poller, EPOLL_CTL_MOD, fds[254], &oneshot));
    CHECK(write(fds[254], &one, sizeof(one)) == sizeof(one));
    CHECK(poll(&descriptor, 1, 0) == 1 && descriptor.revents == POLLIN);
    CHECK(poll(&descriptor, 1, 0) == 1 && descriptor.revents == POLLIN);
    CHECK(epoll_wait(poller, &ready, 1, 0) == 1 && ready.data.u64 == 0x30000);
    CHECK(poll(&descriptor, 1, 0) == 0 && !descriptor.revents);
    CHECK(!epoll_ctl(poller, EPOLL_CTL_MOD, fds[254], &oneshot));
    CHECK(poll(&descriptor, 1, 0) == 1 && descriptor.revents == POLLIN);
    CHECK(epoll_wait(poller, &ready, 1, 0) == 1 && ready.data.u64 == 0x30000);
    CHECK(read(fds[254], &one, sizeof(one)) == sizeof(one) && one == 1);
    int alias = dup(fds[255]), original = fds[255];
    CHECK(alias >= 0 && !close(original));
    fds[255] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(fds[255] == original);
    struct epoll_event replacement = {.events = EPOLLIN, .data.u64 = 0x10000};
    CHECK(!epoll_ctl(poller, EPOLL_CTL_ADD, fds[255], &replacement));
    struct epoll_event second_alias = {.events = EPOLLIN, .data.u64 = 0x20000};
    CHECK(!epoll_ctl(poller, EPOLL_CTL_ADD, alias, &second_alias));
    CHECK(write(alias, &one, sizeof(one)) == sizeof(one));
    struct epoll_event retained[3];
    CHECK(epoll_wait(poller, retained, 3, 0) == 2);
    CHECK((retained[0].data.u64 == 255 && retained[1].data.u64 == 0x20000) ||
          (retained[1].data.u64 == 255 && retained[0].data.u64 == 0x20000));
    CHECK(read(alias, &one, sizeof(one)) == sizeof(one) && one == 1);
    CHECK(write(fds[255], &one, sizeof(one)) == sizeof(one));
    CHECK(epoll_wait(poller, &ready, 1, 0) == 1 && ready.data.u64 == 0x10000);
    CHECK(read(fds[255], &one, sizeof(one)) == sizeof(one) && one == 1);
    CHECK(!close(alias));
    CHECK(epoll_wait(poller, retained, 3, 0) == 0);
    for (unsigned i = 0; i < 256; ++i) CHECK(close(fds[i]) == 0);
    CHECK(close(poller) == 0);
    puts("PASS resources: 256 epoll watches, nonconsuming poll/ONESHOT, dup/close and numeric reuse");
    return 0;
}

static int test_memfd(void)
{
    int fds[80];
    for (unsigned i = 0; i < 80; ++i) {
        fds[i] = syscall(SYS_memfd_create, "resource-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        CHECK(fds[i] >= 0);
    }
    CHECK(ftruncate(fds[0], 16 * 1024 * 1024) == 0);
    unsigned char *shared = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    CHECK(shared != MAP_FAILED && shared[0] == 0 && shared[4096] == 0);
    shared[0] = 0x5a;
    unsigned char *private = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fds[0], 0);
    CHECK(private != MAP_FAILED && private[0] == 0x5a);
    private[0] = 0x7b;
    CHECK(shared[0] == 0x5a);
    errno = 0;
    CHECK(fcntl(fds[0], F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(ftruncate(fds[0], 0) == 0);
    CHECK(ftruncate(fds[0], 16 * 1024 * 1024) == 0);
    CHECK(shared[0] == 0 && private[0] == 0);
    shared[0] = 0x4c;
    CHECK(close(fds[0]) == 0);
    CHECK(shared[0] == 0x4c && private[0] == 0x4c);
    CHECK(munmap(shared, 8192) == 0 && munmap(private, 8192) == 0);
    for (unsigned i = 1; i < 80; ++i) CHECK(close(fds[i]) == 0);
    puts("PASS resources: 80 memfds, sparse size, private COW, seals, truncate and mapping lifetime");
    return 0;
}

static int test_high_memory(void)
{
    void *address = (void *)(uintptr_t)UINT64_C(0x10000000000);
    const size_t span = UINT64_C(1) << 40;
    unsigned char *memory = mmap(address, span, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    CHECK(memory == address);
    memory[0] = 0x13; memory[8192] = 0x37; memory[span - 1] = 0x29;
    int sync[2];
    CHECK(!pipe(sync));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char byte;
        close(sync[1]);
        if (read(sync[0], &byte, 1) != 1 || memory[0] != 0x13 ||
            memory[8192] != 0x37 || memory[span - 1] != 0x29) _exit(1);
        memory[0] = 0x44; memory[span - 1] = 0x77;
        _exit(0);
    }
    CHECK(!close(sync[0]));
    memory[0] = 0x55; memory[span - 1] = 0x66;
    CHECK(write(sync[1], "x", 1) == 1 && !close(sync[1]));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(memory[0] == 0x55 && memory[span - 1] == 0x66);
    CHECK(munmap(memory, span) == 0);
    puts("PASS resources: 1 TiB sparse mapping at 1 TiB, boundary faults and bidirectional fork COW");
    return 0;
}

static int test_sockets_pty(void)
{
    int pairs[80][2], masters[40];
    for (unsigned i = 0; i < 80; ++i) CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pairs[i]) == 0);
    CHECK(write(pairs[79][0], "s", 1) == 1);
    char byte;
    CHECK(read(pairs[79][1], &byte, 1) == 1 && byte == 's');
    for (unsigned i = 0; i < 80; ++i) { CHECK(!close(pairs[i][0])); CHECK(!close(pairs[i][1])); }
    for (unsigned i = 0; i < 40; ++i) { masters[i] = posix_openpt(O_RDWR | O_NOCTTY); CHECK(masters[i] >= 0); }
    int aliases[16];
    for (unsigned i = 0; i < 16; ++i) { aliases[i] = dup(masters[39]); CHECK(aliases[i] >= 0); }
    char name[128];
    CHECK(!grantpt(masters[39]) && !unlockpt(masters[39]));
    CHECK(!ptsname_r(masters[39], name, sizeof(name)));
    int slave = open(name, O_RDWR | O_NOCTTY);
    CHECK(slave >= 0);
    struct termios attributes;
    CHECK(!tcgetattr(slave, &attributes));
    cfmakeraw(&attributes);
    CHECK(!tcsetattr(slave, TCSANOW, &attributes));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) _exit(write(masters[39], "pty", 3) == 3 ? 0 : 1);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    char text[3];
    CHECK(read(slave, text, sizeof(text)) == sizeof(text) && !memcmp(text, "pty", 3));
    int transport[2];
    CHECK(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, transport));
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &masters[39], sizeof(int));
    CHECK(sendmsg(transport[0], &message, 0) == 1);
    memset(&control, 0, sizeof(control));
    message.msg_controllen = sizeof(control.bytes);
    CHECK(recvmsg(transport[1], &message, 0) == 1 && !message.msg_flags);
    header = CMSG_FIRSTHDR(&message);
    CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
          header->cmsg_len == CMSG_LEN(sizeof(int)));
    int received;
    memcpy(&received, CMSG_DATA(header), sizeof(received));
    CHECK(received >= 0);
    for (unsigned i = 0; i < 16; ++i) CHECK(!close(aliases[i]));
    for (unsigned i = 0; i < 40; ++i) CHECK(!close(masters[i]));
    CHECK(write(received, "fd", 2) == 2);
    CHECK(read(slave, text, sizeof(text)) == 2 && !memcmp(text, "fd", 2));
    CHECK(!close(received) && read(slave, text, sizeof(text)) == 0);
    CHECK(!close(slave) && !close(transport[0]) && !close(transport[1]));
    puts("PASS resources: 160 UNIX sockets, 40 PTYs, dup/fork/SCM_RIGHTS and hangup");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit limit;
    CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
    CHECK(limit.rlim_max >= 2048);
    limit.rlim_cur = 2048;
    CHECK(!setrlimit(RLIMIT_NOFILE, &limit));
    CHECK(!test_pipes_select());
    CHECK(!test_epoll());
    CHECK(!test_memfd());
    CHECK(!test_high_memory());
    CHECK(!test_sockets_pty());
    puts("PASS resource capacity Linux ABI runtime probe");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef IN_MASK_CREATE
#define IN_MASK_CREATE 0x10000000U
#endif
#ifndef IN_EXCL_UNLINK
#define IN_EXCL_UNLINK 0x04000000U
#endif

#define MAX_EVENTS 64
#define BUFFER_SIZE 8192

static unsigned checks;
static const char *group;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        fprintf(stderr, "[inotify:%s] FAIL line=%d errno=%d (%s): %s\n", \
                group, __LINE__, errno, strerror(errno), #expr); \
        return 1; \
    } \
} while (0)

struct event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[128];
};

struct batch {
    struct event events[MAX_EVENTS];
    size_t count;
};

static int parse_events(struct batch *batch, const unsigned char *buffer, size_t size)
{
    size_t offset = 0;
    batch->count = 0;
    while (offset < size) {
        struct inotify_event raw;
        struct event *event;
        const char *end;
        if (size - offset < sizeof(raw) || batch->count == MAX_EVENTS)
            goto malformed;
        memcpy(&raw, buffer + offset, sizeof(raw));
        if (raw.len > size - offset - sizeof(raw) ||
            raw.len % sizeof(raw) != 0)
            goto malformed;
        event = &batch->events[batch->count++];
        event->wd = raw.wd;
        event->mask = raw.mask;
        event->cookie = raw.cookie;
        event->len = raw.len;
        event->name[0] = '\0';
        if (raw.len) {
            const char *name = (const char *)buffer + offset + sizeof(raw);
            end = memchr(name, '\0', raw.len);
            if (!end || end == name || (size_t)(end - name) >= sizeof(event->name) ||
                raw.len != ((size_t)(end - name) + 1 + sizeof(raw) - 1) /
                           sizeof(raw) * sizeof(raw))
                goto malformed;
            memcpy(event->name, name, (size_t)(end - name) + 1);
            /* Linux pads the record with zeroes after the terminating NUL. */
            while (++end < name + raw.len)
                if (*end != '\0')
                    goto malformed;
        }
        offset += sizeof(raw) + raw.len;
    }
    return 0;
malformed:
    fprintf(stderr, "[inotify:%s] malformed record at offset %zu of %zu\n",
            group, offset, size);
    return -1;
}

static int read_batch(int fd, struct batch *batch)
{
    unsigned char buffer[BUFFER_SIZE];
    ssize_t size = read(fd, buffer, sizeof(buffer));
    if (size <= 0) {
        fprintf(stderr, "[inotify:%s] read failed: size=%zd errno=%d\n",
                group, size, errno);
        return -1;
    }
    return parse_events(batch, buffer, (size_t)size);
}

static const struct event *find_event(const struct batch *batch, int wd,
                                      uint32_t mask, const char *name)
{
    size_t i;
    for (i = 0; i < batch->count; ++i) {
        const struct event *event = &batch->events[i];
        if (event->wd == wd && event->mask == mask && !strcmp(event->name, name) &&
            ((mask & (IN_MOVED_FROM | IN_MOVED_TO)) || event->cookie == 0))
            return event;
    }
    fprintf(stderr, "[inotify:%s] missing wd=%d mask=%#x name='%s'; got:",
            group, wd, mask, name);
    for (i = 0; i < batch->count; ++i)
        fprintf(stderr, " (%d,%#x,%u,'%s')", batch->events[i].wd,
                batch->events[i].mask, batch->events[i].cookie,
                batch->events[i].name);
    fputc('\n', stderr);
    return NULL;
}

static int empty_queue(int fd)
{
    unsigned char buffer[128];
    ssize_t n;
    errno = 0;
    n = read(fd, buffer, sizeof(buffer));
    return n == -1 && errno == EAGAIN;
}

static int create_file(const char *name)
{
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    return close(fd);
}

static int init_and_errors(void)
{
    int fd, plain, ordinary;
    char data[16];
    group = "init-errors";
    errno = 0;
    CHECK(inotify_init1(0x40000000) == -1 && errno == EINVAL);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(empty_queue(fd));
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == EBADF);
    errno = 0;
    CHECK(pread(fd, data, 1, 0) == -1 && errno == ESPIPE);
    CHECK(lseek(fd, -1, SEEK_SET) == 0 && lseek(fd, 123, SEEK_END) == 0);
    errno = 0;
    CHECK(lseek(fd, 0, 5) == -1 && errno == EINVAL);
    plain = inotify_init();
    CHECK(plain >= 0);
    CHECK((fcntl(plain, F_GETFL) & O_NONBLOCK) == 0);
    CHECK((fcntl(plain, F_GETFD) & FD_CLOEXEC) == 0);
    errno = 0;
    CHECK(inotify_add_watch(fd, ".", 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(inotify_add_watch(fd, "missing", 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(inotify_add_watch(fd, "missing", 0x00800020) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(inotify_add_watch(fd, "missing", IN_OPEN) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(inotify_add_watch(-1, ".", IN_OPEN) == -1 && errno == EBADF);
    errno = 0;
    CHECK(inotify_rm_watch(fd, -1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(inotify_rm_watch(-1, 1) == -1 && errno == EBADF);
    ordinary = open(".", O_RDONLY | O_DIRECTORY);
    CHECK(ordinary >= 0);
    errno = 0;
    CHECK(inotify_add_watch(ordinary, ".", IN_OPEN) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(read(fd, data, 0) == -1 && errno == EAGAIN);
    CHECK(close(ordinary) == 0 && close(plain) == 0 && close(fd) == 0);
    return 0;
}

static int permissions(void)
{
    int status;
    pid_t child;
    group = "permissions";
    CHECK(create_file("denied_file") == 0);
    CHECK(mkdir("denied_dir", 0700) == 0);
    CHECK(chmod("denied_file", 0000) == 0 && chmod("denied_dir", 0111) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int fd;
        if (geteuid() == 0 && (setgid(65534) < 0 || setuid(65534) < 0))
            _exit(77);
        fd = inotify_init1(IN_NONBLOCK);
        if (fd < 0)
            _exit(2);
        errno = 0;
        if (inotify_add_watch(fd, "denied_file", IN_OPEN) != -1 || errno != EACCES)
            _exit(3);
        errno = 0;
        if (inotify_add_watch(fd, "denied_dir", IN_OPEN) != -1 || errno != EACCES)
            _exit(4);
        close(fd);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    if (WEXITSTATUS(status) == 77)
        printf("[inotify:permissions] SKIP cannot drop root privileges\n");
    else
        CHECK(WEXITSTATUS(status) == 0);
    CHECK(chmod("denied_dir", 0700) == 0);
    CHECK(unlink("denied_file") == 0 && rmdir("denied_dir") == 0);
    return 0;
}

static int basic_events(void)
{
    int fd, wd_dir, wd_file, file;
    char byte;
    struct batch b;
    const struct event *event;
    group = "basic-events";
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd_dir = inotify_add_watch(fd, ".", IN_ALL_EVENTS);
    CHECK(wd_dir > 0);
    file = open("basic", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(file >= 0 && write(file, "x", 1) == 1 && close(file) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 4);
    event = find_event(&b, wd_dir, IN_CREATE, "basic");
    CHECK(event && event->cookie == 0 && event->len == 16);
    CHECK(find_event(&b, wd_dir, IN_OPEN, "basic"));
    CHECK(find_event(&b, wd_dir, IN_MODIFY, "basic"));
    CHECK(find_event(&b, wd_dir, IN_CLOSE_WRITE, "basic"));
    wd_file = inotify_add_watch(fd, "basic", IN_ALL_EVENTS);
    CHECK(wd_file > 0 && wd_file != wd_dir);
    CHECK(chmod("basic", 0640) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 2);
    event = find_event(&b, wd_file, IN_ATTRIB, "");
    CHECK(event && event->len == 0 && event->cookie == 0);
    CHECK(find_event(&b, wd_dir, IN_ATTRIB, "basic"));
    file = open("basic", O_RDONLY);
    CHECK(file >= 0 && read(file, &byte, 1) == 1 && byte == 'x' && close(file) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 6);
    CHECK(find_event(&b, wd_file, IN_OPEN, "") && find_event(&b, wd_dir, IN_OPEN, "basic"));
    CHECK(find_event(&b, wd_file, IN_ACCESS, "") && find_event(&b, wd_dir, IN_ACCESS, "basic"));
    CHECK(find_event(&b, wd_file, IN_CLOSE_NOWRITE, "") &&
          find_event(&b, wd_dir, IN_CLOSE_NOWRITE, "basic"));
    CHECK(unlink("basic") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 4);
    CHECK(find_event(&b, wd_file, IN_ATTRIB, ""));
    CHECK(find_event(&b, wd_file, IN_DELETE_SELF, ""));
    CHECK(find_event(&b, wd_file, IN_IGNORED, ""));
    CHECK(find_event(&b, wd_dir, IN_DELETE, "basic"));
    CHECK(mkdir("subdir", 0700) == 0 && rmdir("subdir") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 2);
    CHECK(find_event(&b, wd_dir, IN_CREATE | IN_ISDIR, "subdir"));
    CHECK(find_event(&b, wd_dir, IN_DELETE | IN_ISDIR, "subdir"));
    CHECK(empty_queue(fd) && close(fd) == 0);
    return 0;
}

static int rename_events(void)
{
    int fd, dir_wd, file_wd;
    struct batch b;
    const struct event *from, *to, *self;
    group = "rename";
    CHECK(create_file("before") == 0);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    dir_wd = inotify_add_watch(fd, ".", IN_MOVED_FROM | IN_MOVED_TO);
    file_wd = inotify_add_watch(fd, "before", IN_MOVE_SELF);
    CHECK(dir_wd > 0 && file_wd > 0);
    CHECK(rename("before", "after") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 3);
    from = find_event(&b, dir_wd, IN_MOVED_FROM, "before");
    to = find_event(&b, dir_wd, IN_MOVED_TO, "after");
    self = find_event(&b, file_wd, IN_MOVE_SELF, "");
    CHECK(from && to && self && from->cookie != 0 && from->cookie == to->cookie &&
          self->cookie == 0 && self->len == 0);
    CHECK(unlink("after") == 0 && close(fd) == 0);
    return 0;
}

static int access_completion(void)
{
    int fd, wd, file, directory;
    char byte, entries[4096];
    struct iovec vector = {&byte, 1};
    struct batch b;
    group = "access-completion";
    file = open("access", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(file >= 0 && write(file, "x", 1) == 1 && lseek(file, 0, SEEK_SET) == 0);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, "access", IN_ACCESS);
    CHECK(wd > 0);
    CHECK(read(file, &byte, 0) == 0 && empty_queue(fd));
    CHECK(read(file, &byte, 1) == 1 && byte == 'x');
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS, ""));
    CHECK(read(file, &byte, 1) == 0 && empty_queue(fd));
    CHECK(pread(file, &byte, 1, 1) == 0 && empty_queue(fd));
    CHECK(readv(file, NULL, 0) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS, ""));
    CHECK(readv(file, &vector, 1) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS, ""));
    CHECK(preadv(file, &vector, 1, 1) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS, ""));
    CHECK(close(file) == 0 && close(fd) == 0 && unlink("access") == 0);
    directory = open(".", O_RDONLY | O_DIRECTORY);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(directory >= 0 && fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_ACCESS);
    CHECK(wd > 0);
    CHECK(syscall(SYS_getdents64, directory, entries, sizeof(entries)) > 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS | IN_ISDIR, ""));
    CHECK(syscall(SYS_getdents64, directory, entries, sizeof(entries)) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_ACCESS | IN_ISDIR, ""));
    CHECK(close(directory) == 0 && close(fd) == 0);
    return 0;
}

static int hardlink_and_masks(void)
{
    int fd, wd, file;
    char byte;
    struct batch b;
    group = "hardlink-masks";
    CHECK(create_file("inode") == 0 && link("inode", "alias") == 0);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, "inode", IN_OPEN);
    CHECK(wd > 0);
    CHECK(inotify_add_watch(fd, "alias", IN_ACCESS) == wd);
    CHECK(inotify_add_watch(fd, "inode", IN_MODIFY | IN_MASK_ADD) == wd);
    errno = 0;
    CHECK(inotify_add_watch(fd, "alias", IN_OPEN | IN_MASK_CREATE) == -1 && errno == EEXIST);
    errno = 0;
    CHECK(inotify_add_watch(fd, "alias", IN_OPEN | IN_MASK_CREATE | IN_MASK_ADD) == -1 &&
          errno == EINVAL);
    file = open("alias", O_RDWR);
    CHECK(file >= 0 && write(file, "x", 1) == 1 && lseek(file, 0, SEEK_SET) == 0 &&
          read(file, &byte, 1) == 1 && byte == 'x' && close(file) == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 2);
    CHECK(find_event(&b, wd, IN_MODIFY, "") && find_event(&b, wd, IN_ACCESS, ""));
    CHECK(empty_queue(fd));
    CHECK(close(fd) == 0 && unlink("alias") == 0 && unlink("inode") == 0);
    return 0;
}

static int symlink_flags(void)
{
    int fd, link_wd, target_wd, dir_wd;
    struct batch b;
    group = "symlinks";
    CHECK(create_file("target") == 0 && symlink("target", "slink") == 0);
    CHECK(mkdir("dir_target", 0700) == 0 && symlink("dir_target", "dir_link") == 0);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    link_wd = inotify_add_watch(fd, "slink", IN_MOVE_SELF | IN_DONT_FOLLOW);
    target_wd = inotify_add_watch(fd, "slink", IN_ATTRIB);
    CHECK(link_wd > 0 && target_wd > 0 && link_wd != target_wd);
    errno = 0;
    CHECK(inotify_add_watch(fd, "slink", IN_OPEN | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    errno = 0;
    CHECK(inotify_add_watch(fd, "dir_link", IN_OPEN | IN_ONLYDIR | IN_DONT_FOLLOW) == -1 &&
          errno == ENOTDIR);
    dir_wd = inotify_add_watch(fd, "dir_link", IN_OPEN | IN_ONLYDIR);
    CHECK(dir_wd > 0 && inotify_add_watch(fd, "dir_target", IN_OPEN) == dir_wd);
    CHECK(chmod("target", 0640) == 0 && rename("slink", "slink2") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 2);
    CHECK(find_event(&b, target_wd, IN_ATTRIB, ""));
    CHECK(find_event(&b, link_wd, IN_MOVE_SELF, ""));
    CHECK(close(fd) == 0 && unlink("slink2") == 0 && unlink("target") == 0);
    CHECK(unlink("dir_link") == 0 && rmdir("dir_target") == 0);
    return 0;
}

static int removal_and_oneshot(void)
{
    int fd, wd;
    struct batch b;
    group = "oneshot-removal";
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_CREATE | IN_ONESHOT);
    CHECK(wd > 0 && create_file("one") == 0 && create_file("two") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 2);
    CHECK(find_event(&b, wd, IN_CREATE, "one") && find_event(&b, wd, IN_IGNORED, ""));
    CHECK(empty_queue(fd));
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    CHECK(wd > 0 && inotify_rm_watch(fd, wd) == 0);
    errno = 0;
    CHECK(inotify_rm_watch(fd, wd) == -1 && errno == EINVAL);
    CHECK(create_file("three") == 0);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_IGNORED, ""));
    CHECK(empty_queue(fd));
    CHECK(close(fd) == 0 && unlink("one") == 0 && unlink("two") == 0 && unlink("three") == 0);
    return 0;
}

static int exclude_unlinked(void)
{
    int excl_fd, normal_fd, excl_wd, normal_wd, file;
    struct batch b;
    const uint32_t mask = IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE;
    group = "exclude-unlinked";
    CHECK(create_file("unlinked") == 0);
    file = open("unlinked", O_RDWR);
    CHECK(file >= 0);
    excl_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    normal_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(excl_fd >= 0 && normal_fd >= 0);
    excl_wd = inotify_add_watch(excl_fd, ".", mask | IN_EXCL_UNLINK);
    normal_wd = inotify_add_watch(normal_fd, ".", mask);
    CHECK(excl_wd > 0 && normal_wd > 0);
    CHECK(unlink("unlinked") == 0 && write(file, "x", 1) == 1 && close(file) == 0);
    CHECK(read_batch(excl_fd, &b) == 0 && b.count == 1);
    CHECK(find_event(&b, excl_wd, IN_DELETE, "unlinked") && empty_queue(excl_fd));
    CHECK(read_batch(normal_fd, &b) == 0 && b.count == 3);
    CHECK(find_event(&b, normal_wd, IN_DELETE, "unlinked"));
    CHECK(find_event(&b, normal_wd, IN_MODIFY, "unlinked"));
    CHECK(find_event(&b, normal_wd, IN_CLOSE_WRITE, "unlinked"));
    CHECK(close(excl_fd) == 0 && close(normal_fd) == 0);
    return 0;
}

static int coalescing(void)
{
    int fd, wd, file;
    struct batch b;
    group = "coalescing";
    CHECK(create_file("coalesce") == 0);
    file = open("coalesce", O_WRONLY);
    CHECK(file >= 0);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_MODIFY);
    CHECK(wd > 0);
    CHECK(write(file, "a", 1) == 1 && write(file, "b", 1) == 1);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 &&
          find_event(&b, wd, IN_MODIFY, "coalesce"));
    CHECK(write(file, "c", 1) == 1);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 &&
          find_event(&b, wd, IN_MODIFY, "coalesce"));
    CHECK(close(file) == 0 && close(fd) == 0 && unlink("coalesce") == 0);
    return 0;
}

static int ready_and_io(void)
{
    int fd, wd, ep, count, count_after;
    struct pollfd pfd;
    struct epoll_event interest, returned;
    struct batch b;
    unsigned char buffer[64];
    struct iovec vectors[2];
    ssize_t n;
    volatile uintptr_t bad_address = 1;
    group = "readiness-io";
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    CHECK(wd > 0);
    ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    memset(&interest, 0, sizeof(interest));
    interest.events = EPOLLIN;
    interest.data.fd = fd;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &interest) == 0);
    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    CHECK(poll(&pfd, 1, 0) == 0 && epoll_wait(ep, &returned, 1, 0) == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 0);
    CHECK(create_file("ready") == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 32);
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN));
    CHECK(epoll_wait(ep, &returned, 1, 0) == 1 && (returned.events & EPOLLIN) &&
          returned.data.fd == fd);
    errno = 0;
    CHECK(read(fd, buffer, sizeof(struct inotify_event)) == -1 && errno == EINVAL);
    CHECK(ioctl(fd, FIONREAD, &count_after) == 0 && count_after == count);
    vectors[0].iov_base = buffer; vectors[0].iov_len = 7;
    vectors[1].iov_base = buffer + 7; vectors[1].iov_len = sizeof(buffer) - 7;
    errno = 0;
    CHECK(readv(fd, vectors, 2) == -1 && errno == EINVAL);
    CHECK(ioctl(fd, FIONREAD, &count_after) == 0 && count_after == count);
    CHECK(create_file("ready2") == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 64);
    errno = 0;
    CHECK(read(fd, (void *)bad_address, 32) == -1 && errno == EFAULT);
    CHECK(ioctl(fd, FIONREAD, &count_after) == 0 && count_after == 32);
    CHECK(create_file("ready3") == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 64);
    vectors[0].iov_base = buffer; vectors[0].iov_len = 32;
    vectors[1].iov_base = buffer + 32; vectors[1].iov_len = 32;
    n = readv(fd, vectors, 2);
    CHECK(n == count && parse_events(&b, buffer, (size_t)n) == 0 && b.count == 2);
    CHECK(find_event(&b, wd, IN_CREATE, "ready2") && find_event(&b, wd, IN_CREATE, "ready3"));
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 0) == 0 && epoll_wait(ep, &returned, 1, 0) == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 0 && empty_queue(fd));
    CHECK(close(ep) == 0 && close(fd) == 0 && unlink("ready") == 0 && unlink("ready2") == 0 &&
          unlink("ready3") == 0);
    return 0;
}

static int partial_copy_fault(void)
{
    int fd, wd, count;
    long page;
    unsigned char *memory, *output;
    struct batch b;
    group = "partial-copy-fault";
    page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    memory = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED && mprotect(memory + page, (size_t)page, PROT_NONE) == 0);
    output = memory + page - 32;
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    CHECK(wd > 0 && create_file("fault1") == 0 && create_file("fault2") == 0 &&
          create_file("fault3") == 0);
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 96);
    errno = 0;
    CHECK(read(fd, output, 64) == -1 && errno == EFAULT);
    CHECK(parse_events(&b, output, 32) == 0 && b.count == 1 &&
          find_event(&b, wd, IN_CREATE, "fault1"));
    CHECK(ioctl(fd, FIONREAD, &count) == 0 && count == 32);
    CHECK(read_batch(fd, &b) == 0 && b.count == 1 && find_event(&b, wd, IN_CREATE, "fault3"));
    CHECK(close(fd) == 0 && munmap(memory, (size_t)page * 2) == 0);
    CHECK(unlink("fault1") == 0 && unlink("fault2") == 0 && unlink("fault3") == 0);
    return 0;
}

static int blocking_read(void)
{
    int fd, wd, ready[2], done[2], status, started, early, created, completed, result;
    char marker = 0;
    pid_t child, waited;
    struct pollfd pfd;
    struct batch b;
    group = "blocking-read";
    fd = inotify_init1(IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    CHECK(wd > 0 && pipe(ready) == 0 && pipe(done) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        alarm(3);
        close(ready[0]);
        close(done[0]);
        if (write(ready[1], "s", 1) != 1)
            _exit(2);
        if (read_batch(fd, &b) || b.count != 1 ||
            !find_event(&b, wd, IN_CREATE, "blocking"))
            _exit(3);
        if (write(done[1], "d", 1) != 1)
            _exit(4);
        _exit(0);
    }
    close(ready[1]);
    close(done[1]);
    started = read(ready[0], &marker, 1) == 1 && marker == 's';
    pfd.fd = done[0]; pfd.events = POLLIN; pfd.revents = 0;
    early = poll(&pfd, 1, 50);
    created = create_file("blocking");
    completed = poll(&pfd, 1, 2000);
    result = completed == 1 && read(done[0], &marker, 1) == 1 && marker == 'd';
    waited = waitpid(child, &status, 0);
    close(ready[0]);
    close(done[0]);
    close(fd);
    CHECK(started && early == 0 && created == 0 && completed == 1 && result);
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(unlink("blocking") == 0);
    return 0;
}

static int send_fd(int socket_fd, int fd)
{
    char payload = 'x';
    struct iovec iov = {&payload, 1};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    return sendmsg(socket_fd, &msg, 0) == 1 ? 0 : -1;
}

static int receive_fd(int socket_fd)
{
    char payload;
    int fd = -1;
    struct iovec iov = {&payload, 1};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
    if (recvmsg(socket_fd, &msg, 0) != 1 || (msg.msg_flags & MSG_CTRUNC))
        return -1;
    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(fd)))
        return -1;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static int shared_descriptions(void)
{
    int fd, dupfd, received, wd, status, sockets[2], flags;
    pid_t child;
    struct batch b;
    group = "shared-descriptions";
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0);
    wd = inotify_add_watch(fd, ".", IN_CREATE);
    CHECK(wd > 0);
    dupfd = dup(fd);
    CHECK(dupfd >= 0 && (fcntl(dupfd, F_GETFD) & FD_CLOEXEC) == 0);
    flags = fcntl(dupfd, F_GETFL);
    CHECK(flags >= 0 && fcntl(dupfd, F_SETFL, flags & ~O_NONBLOCK) == 0);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);
    CHECK(fcntl(fd, F_SETFL, flags) == 0 && (fcntl(dupfd, F_GETFL) & O_NONBLOCK));
    CHECK(create_file("forked") == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (read_batch(dupfd, &b) || b.count != 1 ||
            !find_event(&b, wd, IN_CREATE, "forked"))
            _exit(1);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(empty_queue(fd));
    CHECK(close(fd) == 0 && create_file("duplicated") == 0);
    CHECK(read_batch(dupfd, &b) == 0 && b.count == 1 &&
          find_event(&b, wd, IN_CREATE, "duplicated"));
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(send_fd(sockets[0], dupfd) == 0 && close(dupfd) == 0);
    CHECK(create_file("passed") == 0);
    received = receive_fd(sockets[1]);
    CHECK(received >= 0 && (fcntl(received, F_GETFL) & O_NONBLOCK));
    CHECK(read_batch(received, &b) == 0 && b.count == 1 &&
          find_event(&b, wd, IN_CREATE, "passed"));
    CHECK(empty_queue(received));
    CHECK(close(received) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0);
    CHECK(unlink("forked") == 0 && unlink("duplicated") == 0 && unlink("passed") == 0);
    return 0;
}

static void cleanup(void)
{
    static const char *files[] = {
        "denied_file", "basic", "before", "after", "access", "inode", "alias", "target", "slink",
        "slink2", "dir_link", "one", "two", "three", "unlinked", "coalesce", "ready",
        "ready2", "ready3", "fault1", "fault2", "fault3", "blocking", "forked",
        "duplicated", "passed"
    };
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        (void)unlink(files[i]);
    (void)chmod("denied_dir", 0700);
    (void)rmdir("denied_dir");
    (void)rmdir("subdir");
    (void)rmdir("dir_target");
}

int main(int argc, char **argv)
{
    int original, failed;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s NEW_DIRECTORY\n", argv[0]);
        return 2;
    }
    original = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (original < 0 || mkdir(argv[1], 0700) < 0) {
        perror("[inotify] open cwd or create new directory");
        return 2;
    }
    if (chdir(argv[1]) < 0) {
        perror("[inotify] chdir");
        (void)rmdir(argv[1]);
        return 2;
    }
    /* The root-run permission check drops privileges after entering this directory. */
    if (chmod(".", 0755) < 0) {
        perror("[inotify] chmod directory");
        failed = 1;
    } else {
        alarm(30);
        failed = 0;
#define RUN(test) do { \
    int result = test(); \
    if (result) failed = 1; \
    else printf("[inotify:%s] PASS\n", group); \
} while (0)
        RUN(init_and_errors);
        RUN(permissions);
        RUN(basic_events);
        RUN(rename_events);
        RUN(access_completion);
        RUN(hardlink_and_masks);
        RUN(symlink_flags);
        RUN(removal_and_oneshot);
        RUN(exclude_unlinked);
        RUN(coalescing);
        RUN(ready_and_io);
        RUN(partial_copy_fault);
        RUN(blocking_read);
        RUN(shared_descriptions);
#undef RUN
        alarm(0);
    }
    cleanup();
    if (fchdir(original) < 0 || close(original) < 0 || rmdir(argv[1]) < 0) {
        perror("[inotify] cleanup");
        failed = 1;
    }
    if (failed)
        return 1;
    printf("[inotify] PASS checks=%u\n", checks);
    return 0;
}

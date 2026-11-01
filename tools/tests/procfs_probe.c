/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Native Linux-ABI probe: compile with the exported userspace toolchain and run
 * on the guest. Host execution checks the reference ABI and probe itself only. */
static size_t contents(const char *path, char *out, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t n = read(fd, out + used, capacity - used - 1);
        assert(n >= 0);
        if (!n)
            break;
        used += (size_t)n;
    }
    close(fd);
    out[used] = 0;
    return used;
}

static void *thread_probe(void *unused)
{
    (void)unused;
    char link[128] = {0}, expected[128], path[160];
    long tid = syscall(SYS_gettid);
    snprintf(expected, sizeof(expected), "%ld/task/%ld", (long)getpid(), tid);
    ssize_t n = readlink("/proc/thread-self", link, sizeof(link) - 1);
    assert(n > 0 && !strcmp(link, expected));
    snprintf(path, sizeof(path), "/proc/%s/status", expected);
    char text[8192];
    assert(contents(path, text, sizeof(text)) && strstr(text, "Threads:"));
    return NULL;
}

int main(void)
{
    static char text[65536];
    const char *files[] = {"/proc/loadavg",
                           "/proc/meminfo",
                           "/proc/stat",
                           "/proc/vmstat",
                           "/proc/swaps",
                           "/proc/partitions",
                           "/proc/diskstats",
                           "/proc/interrupts",
                           "/proc/net/dev",
                           "/proc/net/route",
                           "/proc/net/arp",
                           "/proc/net/tcp",
                           "/proc/net/udp",
                           "/proc/net/unix",
                           "/proc/sys/fs/nr_open",
                           "/proc/sys/fs/file-nr",
                           "/proc/sys/fs/file-max",
                           "/proc/sys/fs/pipe-max-size",
                           "/proc/sys/fs/pipe-user-pages-soft",
                           "/proc/sys/fs/pipe-user-pages-hard",
                           "/proc/sys/kernel/pid_max",
                           "/proc/self/status",
                           "/proc/self/stat",
                           "/proc/self/statm",
                           "/proc/self/maps",
                           "/proc/self/smaps",
                           "/proc/self/smaps_rollup",
                           "/proc/self/limits",
                           "/proc/self/io"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        int fd = open(files[i], O_RDONLY | O_CLOEXEC);
        assert(fd >= 0);
        assert(read(fd, text, 7) >= 0);
        assert(lseek(fd, 0, SEEK_SET) == 0);
        close(fd);
    }
    assert(contents("/proc/self/maps", text, sizeof(text)) && strstr(text, "r-xp"));
    assert(contents("/proc/self/smaps_rollup", text, sizeof(text)) && strstr(text, "Rss:"));
    assert(contents("/proc/net/dev", text, sizeof(text)) && strstr(text, "lo:"));
    uint64_t auxv[128];
    int fd = open("/proc/self/auxv", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ssize_t bytes = read(fd, auxv, sizeof(auxv));
    close(fd);
    assert(bytes > 0 && !(bytes % 16));
    int pagesize = 0, terminated = 0;
    for (size_t i = 0; i < (size_t)bytes / 8; i += 2) {
        if (auxv[i] == 6) {
            assert(auxv[i + 1] == (uint64_t)sysconf(_SC_PAGESIZE));
            pagesize = 1;
        }
        if (!auxv[i]) {
            terminated = 1;
            break;
        }
    }
    assert(pagesize && terminated);
    fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    int alias = dup(fd);
    assert(alias >= 0);
    char path[128];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    assert(contents(path, text, sizeof(text)) && strstr(text, "pos:") && strstr(text, "flags:"));
    close(fd);
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", alias);
    assert(contents(path, text, sizeof(text)) && strstr(text, "pos:"));
    close(alias);
    DIR *tasks = opendir("/proc/self/task");
    assert(tasks);
    assert(readdir(tasks));
    closedir(tasks);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, thread_probe, NULL));
    assert(!pthread_join(thread, NULL));
    puts("PASS native proc ABI probe: nodes, seek, maps, auxv, descriptors and thread-self");
    return 0;
}

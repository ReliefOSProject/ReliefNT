/* SPDX-License-Identifier: Apache-2.0 */
/* POSIX workload probe: timings are CLOCK_MONOTONIC nanoseconds, not TSC. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PAGE = 4096, KINDS = 5, MAX_WORKERS = 8, MAX_PAGES = 512, MAX_ROUNDS = 8 };
enum { DEMAND, COW, SHARED, FILEMAP, SYSCALL };
static const char *const names[KINDS] = {"demand", "cow", "shared", "filemap", "syscall"};
static unsigned workers = 4, pages = 128, rounds = 4;
static size_t per_worker, total;
static uint64_t *samples;
static volatile unsigned char *cow;
static int file_fd;
static pthread_barrier_t barrier;

static void fail(const char *what)
{
    fprintf(stderr, "smp-latency: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void require(int condition, const char *what)
{
    if (!condition) { errno = EIO; fail(what); }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) fail("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sample(unsigned kind, unsigned worker, size_t n, uint64_t start)
{
    uint64_t end = now_ns();
    require(end >= start, "monotonic clock moved backwards");
    samples[kind * total + worker * per_worker + n] = end - start;
}

static unsigned char pattern(size_t offset)
{
    size_t page = offset / PAGE, within = offset % PAGE;
    unsigned char page_byte = (unsigned char)(page >> (8u * (within % 4u)));
    return (unsigned char)((within * 17u + 31u) ^ page_byte);
}

static unsigned signature(size_t page)
{
    return (unsigned)pattern(page * PAGE) | ((unsigned)pattern(page * PAGE + 1) << 8);
}

static void full_write(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("write");
        p += (size_t)n; length -= (size_t)n;
    }
}

static void check_page(const volatile unsigned char *p, size_t offset)
{
    for (size_t i = 0; i < PAGE; ++i)
        require(p[i] == pattern(offset + i), "file content mismatch");
}

static void process_work(unsigned worker)
{
    size_t bytes = (size_t)pages * PAGE;
    unsigned char buf[PAGE];
    for (unsigned r = 0; r < rounds; ++r) {
        volatile unsigned char *demand = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(demand != MAP_FAILED, "anonymous mmap");
        for (unsigned p = 0; p < pages; ++p) {
            size_t n = (size_t)r * pages + p, offset = (size_t)p * PAGE;
            uint64_t start = now_ns();
            require(demand[offset] == 0 && demand[offset + PAGE - 1] == 0, "demand zero");
            demand[offset] = (unsigned char)(worker + p + 1);
            demand[offset + PAGE - 1] = (unsigned char)(worker + r + 1);
            sample(DEMAND, worker, n, start);
            require(demand[offset] == (unsigned char)(worker + p + 1), "demand write");
            start = now_ns();
            size_t index = n * PAGE;
            require(cow[index] == 0x5a && cow[index + PAGE - 1] == 0xa5, "COW initial");
            cow[index] = (unsigned char)worker;
            cow[index + PAGE - 1] = (unsigned char)r;
            sample(COW, worker, n, start);
            require(cow[index] == (unsigned char)worker && cow[index + PAGE - 1] == (unsigned char)r,
                    "COW write");
        }
        if (munmap((void *)demand, bytes)) fail("anonymous munmap");
        volatile unsigned char *mapped = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, file_fd, 0);
        require(mapped != MAP_FAILED, "file mmap");
        for (unsigned p = 0; p < pages; ++p) {
            size_t n = (size_t)r * pages + p, offset = (size_t)p * PAGE;
            uint64_t start = now_ns();
            check_page(mapped + offset, offset);
            sample(FILEMAP, worker, n, start);
            start = now_ns();
            ssize_t got;
            do { got = pread(file_fd, buf, PAGE, (off_t)offset); } while (got < 0 && errno == EINTR);
            sample(SYSCALL, worker, n, start);
            require(got == PAGE, "pread length");
            check_page(buf, offset);
        }
        if (munmap((void *)mapped, bytes)) fail("file munmap");
    }
}

static void *thread_work(void *arg)
{
    unsigned worker = (unsigned)(uintptr_t)arg;
    int ret = pthread_barrier_wait(&barrier);
    require(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD, "thread barrier");
    for (unsigned r = 0; r < rounds; ++r) {
        size_t bytes = (size_t)pages * PAGE;
        volatile unsigned char *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        require(p != MAP_FAILED, "shared VM mmap");
        for (unsigned i = 0; i < pages; ++i) {
            size_t index = (size_t)i * PAGE;
            uint64_t start = now_ns();
            require(p[index] == 0, "shared VM zero");
            p[index] = (unsigned char)(worker + i + 1);
            sample(SHARED, worker, (size_t)r * pages + i, start);
            require(p[index] == (unsigned char)(worker + i + 1), "shared VM write");
        }
        if (munmap((void *)p, bytes)) fail("shared VM munmap");
    }
    return NULL;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static unsigned number(const char *text, unsigned maximum)
{
    char *end; errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || !value || value > maximum) { errno = EINVAL; fail("argument"); }
    return (unsigned)value;
}

int main(int argc, char **argv)
{
    const char *directory = "/var/tmp";
    if (argc == 2 && !strcmp(argv[1], "--now")) {
        printf("%llu\n", (unsigned long long)now_ns());
        return 0;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) { errno = EINVAL; fail("argument"); }
        if (!strcmp(argv[i], "--workers")) workers = number(argv[i + 1], MAX_WORKERS);
        else if (!strcmp(argv[i], "--pages")) pages = number(argv[i + 1], MAX_PAGES);
        else if (!strcmp(argv[i], "--rounds")) rounds = number(argv[i + 1], MAX_ROUNDS);
        else if (!strcmp(argv[i], "--directory")) directory = argv[i + 1];
        else { errno = EINVAL; fail("argument"); }
    }
    per_worker = (size_t)pages * rounds; total = per_worker * workers;
    samples = mmap(NULL, KINDS * total * sizeof(*samples), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    require(samples != MAP_FAILED, "sample mmap");
    for (size_t i = 0; i < KINDS * total; ++i) samples[i] = UINT64_MAX;
    cow = mmap(NULL, per_worker * PAGE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(cow != MAP_FAILED, "COW mmap");
    for (size_t i = 0; i < per_worker; ++i) { cow[i * PAGE] = 0x5a; cow[i * PAGE + PAGE - 1] = 0xa5; }
    char path[1024];
    require(snprintf(path, sizeof(path), "%s/smp-latency-XXXXXX", directory) < (int)sizeof(path), "path");
    file_fd = mkstemp(path);
    if (file_fd < 0 || unlink(path)) fail("temporary file");
    unsigned char buf[PAGE];
    for (unsigned p = 0; p < pages; ++p) {
        for (size_t i = 0; i < PAGE; ++i) buf[i] = pattern((size_t)p * PAGE + i);
        full_write(file_fd, buf, PAGE);
    }
    for (unsigned p = 0; p < pages; ++p) {
        ssize_t got;
        do { got = pread(file_fd, buf, PAGE, (off_t)p * PAGE); } while (got < 0 && errno == EINTR);
        require(got == PAGE, "warm pread");
        check_page(buf, (size_t)p * PAGE);
    }
    int gate[2];
    if (pipe(gate)) fail("pipe");
    pid_t children[MAX_WORKERS];
    for (unsigned i = 0; i < workers; ++i) {
        children[i] = fork();
        if (children[i] < 0) {
            int saved = errno;
            close(gate[0]); close(gate[1]);
            for (unsigned j = 0; j < i; ++j) {
                int status;
                while (waitpid(children[j], &status, 0) < 0 && errno == EINTR) {}
            }
            errno = saved; fail("fork");
        }
        if (!children[i]) {
            char token; close(gate[1]);
            ssize_t n; do { n = read(gate[0], &token, 1); } while (n < 0 && errno == EINTR);
            if (n != 1) _exit(1);
            process_work(i); _exit(0);
        }
    }
    close(gate[0]);
    for (unsigned i = 0; i < workers; ++i) full_write(gate[1], "x", 1);
    close(gate[1]);
    int failed = 0;
    for (unsigned i = 0; i < workers; ++i) {
        int status; pid_t ret;
        do { ret = waitpid(children[i], &status, 0); } while (ret < 0 && errno == EINTR);
        if (ret != children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    }
    require(!failed, "child workload failed");
    for (size_t i = 0; i < per_worker; ++i)
        require(cow[i * PAGE] == 0x5a && cow[i * PAGE + PAGE - 1] == 0xa5, "parent COW isolation");
    pthread_t threads[MAX_WORKERS];
    require(pthread_barrier_init(&barrier, NULL, workers) == 0, "thread barrier init");
    for (unsigned i = 0; i < workers; ++i)
        require(pthread_create(&threads[i], NULL, thread_work, (void *)(uintptr_t)i) == 0, "pthread_create");
    for (unsigned i = 0; i < workers; ++i)
        require(pthread_join(threads[i], NULL) == 0, "pthread_join");
    require(pthread_barrier_destroy(&barrier) == 0, "thread barrier destroy");
    if (close(file_fd)) fail("close file");
    for (size_t i = 0; i < KINDS * total; ++i)
        require(samples[i] != UINT64_MAX, "missing latency sample");
    printf("NATIVE_PATTERN page0=%u page1=%u page256=%u\n", signature(0), signature(1), signature(256));
    for (unsigned k = 0; k < KINDS; ++k) {
        uint64_t *values = samples + k * total;
        qsort(values, total, sizeof(*values), compare);
#define RANK(p) (((total * (p) + 99) / 100) - 1)
        printf("NATIVE kind=%s count=%zu p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu\n",
               names[k], total, (unsigned long long)values[RANK(50)],
               (unsigned long long)values[RANK(95)], (unsigned long long)values[RANK(99)],
               (unsigned long long)values[total - 1]);
#undef RANK
    }
    if (munmap((void *)cow, per_worker * PAGE) || munmap(samples, KINDS * total * sizeof(*samples)))
        fail("munmap");
    return 0;
}

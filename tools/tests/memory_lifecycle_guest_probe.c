/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded, destructive-to-owned-mappings checks for an actual ReliefNT guest. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096u
#define PEER_STACK (64u * 1024u)
#define SPINS 50000000u
#define LOCK_ONFAULT 1u
#define MAX_EVICTION_BYTES (2ULL * 1024 * 1024 * 1024)

static volatile sig_atomic_t owned_children[2];

/* The watchdog only signals children created by this invocation. */
static void watchdog(int signo)
{
    (void)signo;
    static const char message[] = "LIFECYCLE_FAIL watchdog\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    for (unsigned i = 0; i < 2; ++i)
        if (owned_children[i] > 0) (void)kill((pid_t)owned_children[i], SIGKILL);
    _exit(124);
}

static int fail(const char *name)
{
    fprintf(stderr, "LIFECYCLE_FAIL case=%s errno=%d\n", name, errno);
    return -1;
}

static int reap(pid_t child, unsigned slot)
{
    int status = 0;
    pid_t got;
    do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
    owned_children[slot] = 0;
    if (got != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "LIFECYCLE_CHILD_FAIL pid=%ld status=%d\n", (long)child, status);
        return -1;
    }
    return 0;
}

static unsigned char fingerprint(size_t offset)
{
    return (unsigned char)(((offset * 29u) ^ (offset >> 5) ^ 0xa7u) % 255u + 1u);
}

struct stack_state { volatile unsigned char *page; };
static struct stack_state initial_stack;

static int stack_peer(void *opaque)
{
    struct stack_state *state = opaque;
    if (madvise((void *)state->page, PAGE_BYTES, MADV_DONTNEED)) return 1;
    for (size_t i = 0; i < PAGE_BYTES; ++i) if (state->page[i] != 0) return 2;
    for (size_t i = 0; i < PAGE_BYTES; ++i) state->page[i] = fingerprint(i);
    return 0;
}

/* The page is inside the original process stack; the peer stack is separate.
 * A complete interior page avoids this function's live frame and arguments. */
__attribute__((noinline)) static int test_initial_stack(void)
{
    volatile unsigned char reserve[PAGE_BYTES * 3u];
    uintptr_t start = ((uintptr_t)&reserve[0] + PAGE_BYTES - 1u) & ~(uintptr_t)(PAGE_BYTES - 1u);
    volatile unsigned char *page = (volatile unsigned char *)start;
    for (size_t i = 0; i < PAGE_BYTES; ++i) page[i] = 0x5a;
    void *stack = mmap(NULL, PEER_STACK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return fail("initial_stack_mmap");
    initial_stack.page = page;
    pid_t child = clone(stack_peer, (char *)stack + PEER_STACK,
                        CLONE_VM | SIGCHLD, &initial_stack);
    if (child < 0) { munmap(stack, PEER_STACK); return fail("initial_stack_clone"); }
    owned_children[0] = child;
    int result = reap(child, 0);
    if (!result) for (size_t i = 0; i < PAGE_BYTES; ++i)
        if (page[i] != fingerprint(i)) { result = fail("initial_stack_readback"); break; }
    if (munmap(stack, PEER_STACK)) result = fail("initial_stack_cleanup");
    if (!result) puts("LIFECYCLE_PASS case=initial_stack_clone_vm discard_zero=1 readback=1");
    return result;
}

static int test_forks(void)
{
    const size_t size = 2u * PAGE_BYTES;
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("private_mmap");
    for (size_t i = 0; i < size; ++i) p[i] = fingerprint(i);
    pid_t child = fork();
    if (child < 0) { munmap(p, size); return fail("private_fork"); }
    if (!child) {
        for (size_t i = 0; i < size; ++i) if (p[i] != fingerprint(i)) _exit(1);
        for (size_t i = 0; i < PAGE_BYTES; ++i) p[i] ^= 0x5a;
        for (size_t i = 0; i < PAGE_BYTES; ++i) if (p[i] != (unsigned char)(fingerprint(i) ^ 0x5a)) _exit(2);
        _exit(0);
    }
    owned_children[0] = child;
    int result = reap(child, 0);
    if (!result) for (size_t i = 0; i < size; ++i)
        if (p[i] != fingerprint(i)) { result = fail("private_parent_cow"); break; }
    if (munmap(p, size)) result = fail("private_cleanup");
    if (result) return result;
    puts("LIFECYCLE_PASS case=private_fork_cow bytes=8192");

    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("shared_mmap");
    for (size_t i = 0; i < size; ++i) p[i] = fingerprint(i);
    child = fork();
    if (child < 0) { munmap(p, size); return fail("shared_fork"); }
    if (!child) {
        p[17] = 0x69;
        if (madvise(p + PAGE_BYTES, PAGE_BYTES, MADV_DONTNEED)) _exit(3);
        for (size_t i = PAGE_BYTES; i < size; ++i) if (p[i] != fingerprint(i)) _exit(4);
        p[PAGE_BYTES + 23] = 0xb4;
        _exit(0);
    }
    owned_children[0] = child;
    result = reap(child, 0);
    if (!result && (p[17] != 0x69 || p[PAGE_BYTES + 23] != 0xb4))
        result = fail("shared_parent_visibility");
    if (!result) for (size_t i = 0; i < size; ++i) {
        unsigned char expected = i == 17 ? 0x69 : i == PAGE_BYTES + 23 ? 0xb4 : fingerprint(i);
        if (p[i] != expected) { result = fail("shared_fingerprint"); break; }
    }
    if (munmap(p, size)) result = fail("shared_cleanup");
    if (!result) puts("LIFECYCLE_PASS case=shared_anon_fork discard_refault=1 bytes=8192");
    return result;
}

static int test_checked_lock(void)
{
    const size_t size = 64u * PAGE_BYTES;
    unsigned char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("checked_lock_mmap");
    int result = -1;
    unsigned char residency[64]; /* Separate from the inspected mapping. */
    if (mprotect(p + 63u * PAGE_BYTES, PAGE_BYTES, PROT_NONE)) goto out;
    if (syscall(SYS_mlock2, p, 2u * PAGE_BYTES, LOCK_ONFAULT)) goto out;
    errno = 0;
    if (mlock(p, size) != -1 || errno != ENOMEM) goto out;
    if (madvise(p + 10u * PAGE_BYTES, PAGE_BYTES, MADV_DONTNEED)) goto out;
    errno = 0;
    if (madvise(p, PAGE_BYTES, MADV_DONTNEED) != -1 || errno != EINVAL) goto out;
    if (mprotect(p + 63u * PAGE_BYTES, PAGE_BYTES, PROT_READ | PROT_WRITE)) goto out;
    if (mlock(p, size) || mincore(p, size, residency)) goto out;
    for (size_t i = 0; i < 64; ++i) if (!(residency[i] & 1)) goto out;
    result = 0;
out:
    if (result) fail("checked_lock");
    if (munlock(p, size) && !result) result = fail("checked_unlock");
    if (munmap(p, size) && !result) result = fail("checked_lock_cleanup");
    if (!result) puts("LIFECYCLE_PASS case=checked_lock failure_preserved_prior_lock=1 resident_after_success=64 vmlck=unavailable");
    return result;
}

static int test_protected_move(void)
{
    const size_t size = 2u * PAGE_BYTES;
    unsigned char *src = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) return fail("move_source");
    unsigned char *dst = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dst == MAP_FAILED) { munmap(src, size); return fail("move_destination"); }
    int result = -1;
    if (syscall(SYS_mlock2, src, size, LOCK_ONFAULT)) goto out;
    for (size_t i = 0; i < size; ++i) src[i] = fingerprint(i);
    if (mprotect(src, size, PROT_NONE)) goto out;
    void *moved = mremap(src, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    if (moved == MAP_FAILED || moved != dst) goto out;
    src = MAP_FAILED;
    if (mprotect(dst, size, PROT_READ | PROT_WRITE)) goto out;
    for (size_t i = 0; i < size; ++i) if (dst[i] != fingerprint(i)) goto out;
    errno = 0;
    if (madvise(dst, PAGE_BYTES, MADV_DONTNEED) != -1 || errno != EINVAL) goto out;
    result = 0;
out:
    if (result) fail("protected_move");
    if (src != MAP_FAILED) { (void)munlock(src, size); (void)munmap(src, size); }
    (void)munlock(dst, size);
    if (munmap(dst, size) && !result) result = fail("move_cleanup");
    if (!result) puts("LIFECYCLE_PASS case=protected_move locked=1 bytes=8192 permissions_restored=1");
    return result;
}

static int transfer_byte(int fd, unsigned char *byte, int writing)
{
    ssize_t count;
    do { count = writing ? write(fd, byte, 1) : read(fd, byte, 1); }
    while (count < 0 && errno == EINTR);
    return count == 1 ? 0 : -1;
}

/* Wait until the parent has checked an empty pipe, then publish the byte after
 * a bounded delay so the caller can observe the user-visible wait and wakeup. */
static int delayed_pipe_writer(int gate, int output, unsigned char marker)
{
    unsigned char token;
    if (transfer_byte(gate, &token, 0) || token != 0x73 || close(gate)) return -1;
    struct timespec delay = {0, 200000000};
    while (nanosleep(&delay, &delay)) if (errno != EINTR) return -1;
    return transfer_byte(output, &marker, 1);
}

static int pipe_wait_ready(int fd, int use_pselect, int timeout_ms)
{
    int ready;
    if (use_pselect) {
        if (fd >= FD_SETSIZE) { errno = EINVAL; return -1; }
        fd_set readfds;
        FD_ZERO(&readfds); FD_SET(fd, &readfds);
        const struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        sigset_t mask; sigemptyset(&mask);
        ready = pselect(fd + 1, &readfds, NULL, NULL, &timeout, &mask);
        if (ready == 1 && !FD_ISSET(fd, &readfds)) { errno = EIO; return -1; }
    } else {
        struct pollfd waitfd = {fd, POLLIN, 0};
        ready = poll(&waitfd, 1, timeout_ms);
        if (ready == 1 && !(waitfd.revents & POLLIN)) { errno = EIO; return -1; }
    }
    return ready;
}

static int test_pipe_wait(int use_pselect, int previous_pipe[2])
{
    const char *name = use_pselect ? "pselect_pipe" : "poll_pipe";
    int data[2] = {-1, -1}, gate[2] = {-1, -1};
    pid_t child = -1;
    int result = -1, saved_errno = 0;
    int64_t elapsed_ns = 0;
    unsigned char marker = use_pselect ? 0x5a : 0xa5, received = 0, token = 0x73;
    if (pipe(data)) goto out;
    if (previous_pipe[0] >= 0 &&
        (data[0] != previous_pipe[0] || data[1] != previous_pipe[1])) {
        errno = EIO; goto out;
    }
    previous_pipe[0] = data[0]; previous_pipe[1] = data[1];
    if (pipe(gate)) goto out;
    child = fork();
    if (child < 0) goto out;
    if (!child) {
        (void)close(data[0]); (void)close(gate[1]);
        int rc = delayed_pipe_writer(gate[0], data[1], marker);
        if (close(data[1])) rc = -1;
        _exit(rc ? 1 : 0);
    }
    owned_children[0] = child;
    int closed = close(data[1]); data[1] = -1;
    if (closed) goto out;
    closed = close(gate[0]); gate[0] = -1;
    if (closed) goto out;
    int ready = pipe_wait_ready(data[0], use_pselect, 0);
    if (ready != 0) { if (ready > 0) errno = EIO; goto out; }
    if (transfer_byte(gate[1], &token, 1)) goto out;
    closed = close(gate[1]); gate[1] = -1;
    if (closed) goto out;
    struct timespec before, after;
    if (clock_gettime(CLOCK_MONOTONIC, &before)) goto out;
    ready = pipe_wait_ready(data[0], use_pselect, 5000);
    if (ready != 1) { if (ready == 0) errno = ETIMEDOUT; goto out; }
    if (clock_gettime(CLOCK_MONOTONIC, &after)) goto out;
    if (transfer_byte(data[0], &received, 0) || received != marker) { errno = EIO; goto out; }
    elapsed_ns = (int64_t)(after.tv_sec - before.tv_sec) * 1000000000LL +
                 after.tv_nsec - before.tv_nsec;
    if (elapsed_ns < 20000000LL) {
        fprintf(stderr, "LIFECYCLE_NOTE case=%s wait_too_short_ns=%" PRId64 "\n", name, elapsed_ns);
        errno = EIO; goto out;
    }
    int status = reap(child, 0); child = -1;
    if (status) { errno = EIO; goto out; }
    result = 0;
out:
    if (result) saved_errno = errno;
    if (child > 0) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        owned_children[0] = 0;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (data[i] >= 0 && close(data[i]) && !result) { result = -1; saved_errno = errno; }
        if (gate[i] >= 0 && close(gate[i]) && !result) { result = -1; saved_errno = errno; }
    }
    if (result) { errno = saved_errno; return fail(name); }
    printf("LIFECYCLE_PASS case=%s empty_not_ready=1 wait_ms=%" PRId64 " wake=1%s\n", name,
           (int64_t)(elapsed_ns / 1000000), use_pselect ? " pipe_fds_reused=1" : "");
    return 0;
}

static int proc_counter(const char *path, const char *name, uint64_t *out)
{
    /* musl fopen("r") opens O_RDONLY first, then calls fdopen for the same
     * FILE plus stdio buffer. Keep that order while identifying the failure. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int saved_errno = errno;
        fprintf(stderr, "LIFECYCLE_PROC path=%s key=%s stage=open errno=%d\n",
                path, name, saved_errno);
        errno = saved_errno; return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        int saved_errno = errno;
        (void)close(fd);
        fprintf(stderr, "LIFECYCLE_PROC path=%s key=%s stage=fdopen errno=%d\n",
                path, name, saved_errno);
        errno = saved_errno; return -1;
    }
    char line[256], key[64];
    unsigned long long value;
    int found = 0, read_failed = 0, read_errno = 0;
    for (;;) {
        if (!fgets(line, sizeof(line), file)) {
            read_errno = errno;
            read_failed = ferror(file);
            break;
        }
        if (sscanf(line, "%63s %llu", key, &value) == 2 && !strcmp(key, name)) {
            *out = (uint64_t)value; found = 1; break;
        }
    }
    int close_result = fclose(file);
    int close_errno = errno;
    if (read_failed)
        fprintf(stderr, "LIFECYCLE_PROC path=%s key=%s stage=fgets errno=%d ferror=1\n",
                path, name, read_errno);
    else if (!found)
        fprintf(stderr, "LIFECYCLE_PROC path=%s key=%s stage=missing_field errno=0\n",
                path, name);
    if (close_result)
        fprintf(stderr, "LIFECYCLE_PROC path=%s key=%s stage=fclose errno=%d\n",
                path, name, close_errno);
    if (read_failed || !found || close_result) {
        errno = read_failed ? read_errno : !found ? 0 : close_errno;
        return -1;
    }
    return 0;
}

/* No force-eviction ABI: a written private page must be nonresident in
 * mincore immediately before each operation. System counters are snapshots. */
static int test_evicted_pages(size_t bytes)
{
    printf("LIFECYCLE_STAGE case=evicted_pages stage=begin bytes=%zu\n", bytes);
    uint64_t swap_kib, before, after;
    if (proc_counter("/proc/meminfo", "SwapTotal:", &swap_kib) || !swap_kib ||
        proc_counter("/proc/vmstat", "pswpout", &before)) {
        puts("LIFECYCLE_SKIP case=evicted_pages reason=swap_or_counters_unavailable"); return 1;
    }
    size_t pages = bytes / PAGE_BYTES;
    unsigned char *residency = malloc(pages);
    if (!residency) return fail("eviction_mincore_buffer");
    unsigned char *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { free(residency); return fail("eviction_mmap"); }
    int result = -1;
    const char *failure = "evicted_pages";
    void *destination = MAP_FAILED;
    puts("LIFECYCLE_STAGE case=evicted_pages stage=touch_begin");
    size_t midpoint = bytes / 2;
    for (size_t i = 0; i < midpoint; ++i) memory[i] = fingerprint(i);
    puts("LIFECYCLE_STAGE case=evicted_pages stage=touch_half");
    for (size_t i = midpoint; i < bytes; ++i) memory[i] = fingerprint(i);
    puts("LIFECYCLE_STAGE case=evicted_pages stage=touch_complete");
    failure = "eviction_mincore_all";
    if (mincore(memory, bytes, residency)) goto out;
    failure = "eviction_vmstat_after";
    if (proc_counter("/proc/vmstat", "pswpout", &after)) goto out;
    size_t first = pages, second = pages, nonresident = 0;
    for (size_t i = 0; i < pages; ++i) if (!(residency[i] & 1)) {
        ++nonresident;
        if (first == pages) first = i;
        else if (second == pages) second = i;
    }
    printf("LIFECYCLE_EVICTION pages=%zu nonresident=%zu pswpout_before=%" PRIu64
           " pswpout_after=%" PRIu64 "\n", pages, nonresident, before, after);
    if (second == pages || after <= before) {
        puts("LIFECYCLE_SKIP case=evicted_pages reason=insufficient_observed_eviction");
        result = 1; goto out;
    }
    unsigned char current = 0;
    failure = "eviction_first_mincore";
    if (mincore(memory + first * PAGE_BYTES, PAGE_BYTES, &current)) goto out;
    if (current & 1) {
        puts("LIFECYCLE_SKIP case=evicted_pages reason=first_page_became_resident");
        result = 1; goto out;
    }
    failure = "eviction_fork";
    pid_t child = fork();
    if (child < 0) goto out;
    if (!child) {
        unsigned char *page = memory + first * PAGE_BYTES;
        for (size_t i = 0; i < PAGE_BYTES; ++i)
            if (page[i] != fingerprint(first * PAGE_BYTES + i)) _exit(1);
        for (size_t i = 0; i < PAGE_BYTES; ++i) page[i] ^= 0x5a;
        for (size_t i = 0; i < PAGE_BYTES; ++i)
            if (page[i] != (unsigned char)(fingerprint(first * PAGE_BYTES + i) ^ 0x5a)) _exit(2);
        _exit(0);
    }
    owned_children[0] = child;
    failure = "eviction_fork_wait";
    if (reap(child, 0)) goto out;
    failure = "eviction_fork_parent_readback";
    for (size_t i = 0; i < PAGE_BYTES; ++i)
        if (memory[first * PAGE_BYTES + i] != fingerprint(first * PAGE_BYTES + i)) {
            errno = 0; goto out;
        }
    puts("LIFECYCLE_PASS case=evicted_fork_cow nonresident_before_fork=1 bytes=4096");

    failure = "eviction_rescan_mincore";
    if (mincore(memory, bytes, residency)) goto out;
    second = pages;
    for (size_t i = 0; i < pages; ++i) if (!(residency[i] & 1)) { second = i; break; }
    if (second == pages) {
        puts("LIFECYCLE_SKIP case=evicted_protected_move reason=no_nonresident_page");
        result = 1; goto out;
    }
    failure = "eviction_destination_mmap";
    destination = mmap(NULL, PAGE_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (destination == MAP_FAILED) goto out;
    failure = "eviction_source_protect_none";
    if (mprotect(memory + second * PAGE_BYTES, PAGE_BYTES, PROT_NONE)) goto out;
    failure = "eviction_protected_mincore";
    if (mincore(memory + second * PAGE_BYTES, PAGE_BYTES, &current)) goto out;
    if (current & 1) {
        puts("LIFECYCLE_SKIP case=evicted_protected_move reason=page_became_resident");
        result = 1; goto out;
    }
    failure = "eviction_mremap";
    if (mremap(memory + second * PAGE_BYTES, PAGE_BYTES, PAGE_BYTES,
               MREMAP_MAYMOVE | MREMAP_FIXED, destination) != destination) goto out;
    failure = "eviction_destination_restore";
    if (mprotect(destination, PAGE_BYTES, PROT_READ | PROT_WRITE)) goto out;
    failure = "eviction_destination_readback";
    for (size_t i = 0; i < PAGE_BYTES; ++i)
        if (((unsigned char *)destination)[i] != fingerprint(second * PAGE_BYTES + i)) {
            errno = 0; goto out;
        }
    puts("LIFECYCLE_PASS case=evicted_protected_move nonresident_before_move=1 bytes=4096");
    result = 0;
out:
    if (result < 0) fail(failure);
    if (destination != MAP_FAILED && munmap(destination, PAGE_BYTES) && result >= 0)
        result = fail("eviction_destination_cleanup");
    if (munmap(memory, bytes) && result >= 0) result = fail("eviction_mapping_cleanup");
    free(residency);
    return result;
}

struct file_peer { volatile unsigned *ready, *go, *live; volatile unsigned char *page; unsigned cpu, index, observed_cpu; };
static struct file_peer file_peers[2];
static volatile unsigned file_ready, file_go, file_live[2];

static int can_pin(unsigned cpu)
{
    uint64_t mask = UINT64_C(1) << cpu;
    if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask)) return 0;
    for (unsigned n = 0; n < 10000; ++n) {
        unsigned actual = UINT32_MAX;
        if (syscall(SYS_getcpu, &actual, NULL, NULL)) return 0;
        if (actual == cpu) return 1;
        sched_yield();
    }
    return 0;
}

static int file_peer(void *opaque)
{
    struct file_peer *state = opaque;
    uint64_t mask = UINT64_C(1) << state->cpu;
    if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask)) return 1;
    unsigned cpu = UINT32_MAX;
    for (unsigned n = 0; n < 10000; ++n) {
        if (syscall(SYS_getcpu, &cpu, NULL, NULL)) return 2;
        if (cpu == state->cpu) break;
        sched_yield();
    }
    if (cpu != state->cpu) return 3;
    state->observed_cpu = cpu;
    if (state->page[state->index] != fingerprint(state->index)) return 4;
    __atomic_fetch_add(state->ready, 1u, __ATOMIC_RELEASE);
    unsigned n;
    for (n = 0; n < SPINS && !__atomic_load_n(state->go, __ATOMIC_ACQUIRE); ++n)
        __asm__ volatile("pause" ::: "memory");
    if (n == SPINS) return 5;
    __atomic_store_n(&state->live[state->index], 1u, __ATOMIC_RELEASE);
    for (n = 0; n < SPINS && !__atomic_load_n(&state->live[1u - state->index], __ATOMIC_ACQUIRE); ++n)
        __asm__ volatile("pause" ::: "memory");
    if (n == SPINS) return 6;
    state->page[state->index] = state->index ? 0xc3 : 0x3c;
    return 0;
}

/* Return 0 for a pass, 1 for a documented affinity/CPU skip, -1 for failure. */
static int test_shared_file(const char *path)
{
    uint64_t mask = 0;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask) != (long)sizeof(mask)) {
        puts("LIFECYCLE_SKIP case=shared_file_smp reason=affinity_unavailable"); return 1;
    }
    unsigned cpus[2] = { UINT32_MAX, UINT32_MAX }, found = 0;
    for (unsigned i = 0; i < 64 && found < 2; ++i) if (mask & (UINT64_C(1) << i)) cpus[found++] = i;
    if (found < 2) { puts("LIFECYCLE_SKIP case=shared_file_smp reason=fewer_than_two_cpus"); return 1; }
    int pinning = can_pin(cpus[0]) && can_pin(cpus[1]);
    if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask)) return fail("restore_affinity");
    if (!pinning) { puts("LIFECYCLE_SKIP case=shared_file_smp reason=pinning_unavailable"); return 1; }
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return fail("shared_file_create");
    struct stat owned;
    int have_owned = 0;
    int result = -1;
    void *mapping = MAP_FAILED, *stacks[2] = { MAP_FAILED, MAP_FAILED };
    pid_t children[2] = { -1, -1 };
    unsigned char bytes[PAGE_BYTES];
    if (fstat(fd, &owned)) goto out;
    have_owned = 1;
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = fingerprint(i);
    size_t written = 0;
    while (written < sizeof(bytes)) {
        ssize_t n = write(fd, bytes + written, sizeof(bytes) - written);
        if (n <= 0) goto out;
        written += (size_t)n;
    }
    mapping = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) goto out;
    file_ready = file_go = file_live[0] = file_live[1] = 0;
    for (unsigned i = 0; i < 2; ++i) {
        stacks[i] = mmap(NULL, PEER_STACK, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stacks[i] == MAP_FAILED) goto out;
        file_peers[i] = (struct file_peer){ &file_ready, &file_go, file_live,
                                          mapping, cpus[i], i, UINT32_MAX };
        children[i] = clone(file_peer, (char *)stacks[i] + PEER_STACK,
                            CLONE_VM | SIGCHLD, &file_peers[i]);
        if (children[i] < 0) goto out;
        owned_children[i] = children[i];
    }
    unsigned n;
    for (n = 0; n < SPINS && __atomic_load_n(&file_ready, __ATOMIC_ACQUIRE) != 2u; ++n)
        sched_yield();
    if (n == SPINS) goto out;
    __atomic_store_n(&file_go, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; ++i) {
        int status = reap(children[i], i);
        children[i] = -1;
        if (status) goto out;
    }
    if (((unsigned char *)mapping)[0] != 0x3c || ((unsigned char *)mapping)[1] != 0xc3) goto out;
    for (size_t i = 2; i < PAGE_BYTES; ++i)
        if (((unsigned char *)mapping)[i] != fingerprint(i)) goto out;
    result = 0;
out:
    if (result) fail("shared_file_smp");
    __atomic_store_n(&file_go, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2; ++i) if (children[i] > 0) {
        (void)kill(children[i], SIGKILL); (void)reap(children[i], i);
    }
    for (unsigned i = 0; i < 2; ++i) if (stacks[i] != MAP_FAILED) (void)munmap(stacks[i], PEER_STACK);
    if (mapping != MAP_FAILED && munmap(mapping, PAGE_BYTES) && !result) result = fail("shared_file_unmap");
    struct stat current;
    if (have_owned && !lstat(path, &current) && current.st_dev == owned.st_dev && current.st_ino == owned.st_ino) {
        if (unlink(path) && !result) result = fail("shared_file_unlink");
    } else if (!result) result = fail("shared_file_identity");
    if (close(fd) && !result) result = fail("shared_file_close");
    if (!result) {
        printf("LIFECYCLE_PASS case=shared_file_smp cpu0=%u cpu1=%u both_ready=1 bytes=4096\n",
               file_peers[0].observed_cpu, file_peers[1].observed_cpu);
        puts("LIFECYCLE_NOTE shared_file_exact_fault_overlap=unproven remote_tlb=unproven");
    }
    return result;
}

int main(int argc, char **argv)
{
    struct utsname system;
    if ((argc != 4 && argc != 6) || strcmp(argv[1], "--guest-lifecycle") || uname(&system) ||
        strcmp(system.sysname, "ReliefNT")) {
        fprintf(stderr, "usage on ReliefNT guest only: %s --guest-lifecycle OWNED_NEW_EXT_FILE TIMEOUT_SECONDS [--eviction-bytes BYTES]\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    unsigned long timeout = strtoul(argv[3], &end, 10);
    if (errno || end == argv[3] || *end || argv[3][0] == '-' || timeout < 10 || timeout > 1800 ||
        sysconf(_SC_PAGESIZE) != PAGE_BYTES) return 2;
    size_t eviction_bytes = 0;
    if (argc == 6) {
        if (strcmp(argv[4], "--eviction-bytes")) return 2;
        errno = 0;
        unsigned long long value = strtoull(argv[5], &end, 10);
        if (errno || end == argv[5] || *end || argv[5][0] == '-' || value < 2u * PAGE_BYTES ||
            value > MAX_EVICTION_BYTES || value > SIZE_MAX || value % PAGE_BYTES) return 2;
        eviction_bytes = (size_t)value;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction action = {0}; action.sa_handler = watchdog; sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) { fail("watchdog_install"); return 1; }
    alarm((unsigned)timeout);
    /* Must precede every lock-policy operation that could materialize stack VMAs. */
    int previous_pipe[2] = {-1, -1};
    if (test_initial_stack() || test_forks() || test_checked_lock() || test_protected_move() ||
        test_pipe_wait(0, previous_pipe) || test_pipe_wait(1, previous_pipe)) return 1;
    int file_result = test_shared_file(argv[2]);
    if (file_result < 0) return 1;
    int eviction_result = eviction_bytes ? test_evicted_pages(eviction_bytes) : 1;
    if (eviction_result < 0) return 1;
    alarm(0);
    if (!eviction_bytes) puts("LIFECYCLE_SKIP case=evicted_pages reason=not_requested");
    printf("LIFECYCLE_DONE shared_file_smp=%s evicted_pages=%s\n",
           file_result ? "skipped" : "passed", eviction_result ? "skipped" : "passed");
    return 0;
}

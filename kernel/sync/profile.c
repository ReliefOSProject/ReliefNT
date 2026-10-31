/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/lock_profile.h>
#include <reliefnt/heap.h>
#include <reliefnt/smp.h>
#include <reliefnt/syscall.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>

/* Upper 32 bits identify a reset window, bits 1..31 an enable session and bit
 * zero the switch. Neither generation wraps: control fails with EOVERFLOW. */
static uint64_t profile_state = 1ULL << 32;
struct profile_bucket { uint64_t epoch; struct lock_profile_stats value; };
struct profile_active { uint64_t state, start, acquired; uint32_t scope; };
static struct profile_bucket buckets[SMP_MAX_CPUS][LOCK_SCOPE_COUNT];
static struct profile_active active[SMP_MAX_CPUS];

#ifdef RELIEFNT_LOCK_PROFILE_TEST
/** @brief Obtain fixture-supplied ticks in host test builds.
 * @return Deterministic test tick value. Only linked in host fixtures. */
extern uint64_t lock_profile_test_clock(void);
/** @brief Read the deterministic host fixture clock.
 * @return Fixture-provided ticks. Test build only; callable with IRQs masked. */
static uint64_t profile_clock(void) { return lock_profile_test_clock(); }
#else
/** @brief Read ordered raw TSC cycles without a clock subsystem lock.
 * @return Uncalibrated TSC ticks; never a nanosecond value. IRQs are masked. */
static uint64_t profile_clock(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}
#endif

/** @brief Load a counter atomically alongside concurrent snapshot readers.
 * @param p Required borrowed pointer to an aligned counter.
 * @return Its current value. IRQ-safe; no ownership transfer or allocation. */
static uint64_t load_u64(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
/** @brief Store a counter atomically alongside concurrent snapshot readers.
 * @param p Required borrowed pointer to an aligned writable counter.
 * @param value New counter value. Only its CPU writes; IRQs remain masked. */
static void store_u64(uint64_t *p, uint64_t value) { __atomic_store_n(p, value, __ATOMIC_RELAXED); }
/** @brief Add to a counter, saturating rather than wrapping.
 * @param p Required borrowed pointer to this CPU's writable counter.
 * @param value Nonnegative increment. IRQs are masked; snapshots may read. */
static void add_sat(uint64_t *p, uint64_t value)
{
    uint64_t old = load_u64(p), next;
    do { next = UINT64_MAX - old < value ? UINT64_MAX : old + value; }
    while (!__atomic_compare_exchange_n(p, &old, next, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}
/** @brief Retain the maximum without exposing a torn value to readers.
 * @param p Required borrowed pointer to this CPU's writable maximum.
 * @param value Candidate value. IRQs are masked; snapshots may read. */
static void max_u64(uint64_t *p, uint64_t value)
{
    uint64_t old = load_u64(p);
    while (old < value && !__atomic_compare_exchange_n(p, &old, value, false,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/** @brief Map a raw cycle duration to a bounded base-two histogram bin.
 * @param value Duration in raw TSC cycles.
 * @return Index 0 through LOCK_PROFILE_BINS-1; final bin includes overflow.
 * Pure IRQ-safe calculation without allocation or ownership transfer. */
static uint32_t bucket_index(uint64_t value)
{
    uint32_t index = 0;
    while (value > 1 && index + 1 < LOCK_PROFILE_BINS) { value >>= 1; ++index; }
    return index;
}
#define PROFILE_SCALARS(X) \
    X(acquisitions) X(contentions) X(wait_total) X(wait_max) \
    X(hold_total) X(hold_max) X(irq_total) X(irq_max) X(clock_errors)

/** @brief Clear old-window counters before publishing the new bucket epoch.
 * @param value Required borrowed CPU-local writable counter block.
 * IRQs are masked; fields are stored atomically for concurrent snapshots. */
static void clear_values(struct lock_profile_stats *value)
{
#define CLEAR(name) store_u64(&value->name, 0);
    PROFILE_SCALARS(CLEAR)
#undef CLEAR
    for (uint32_t i = 0; i < LOCK_PROFILE_BINS; ++i) {
        store_u64(&value->wait_hist[i], 0); store_u64(&value->hold_hist[i], 0);
        store_u64(&value->irq_hist[i], 0);
    }
}

/** @brief Copy individually atomic counters from one CPU bucket.
 * @param out Required caller-owned writable output block.
 * @param value Required borrowed source, which another CPU may be updating.
 * No global point-in-time guarantee, allocation or ownership transfer. */
static void copy_values(struct lock_profile_stats *out, const struct lock_profile_stats *value)
{
#define COPY(name) out->name = load_u64(&value->name);
    PROFILE_SCALARS(COPY)
#undef COPY
    for (uint32_t i = 0; i < LOCK_PROFILE_BINS; ++i) {
        out->wait_hist[i] = load_u64(&value->wait_hist[i]);
        out->hold_hist[i] = load_u64(&value->hold_hist[i]);
        out->irq_hist[i] = load_u64(&value->irq_hist[i]);
    }
}

/** @brief Select a CPU scope bucket and lazily enter the requested epoch.
 * @param cpu Valid current CPU index with local IRQs masked.
 * @param scope Valid fixed scope ID.
 * @param state Current captured session.
 * @return Borrowed writable bucket counters. This CPU is the sole writer;
 * old fields are atomically cleared before publishing the new epoch. */
static struct lock_profile_stats *bucket_for(uint32_t cpu, uint32_t scope, uint64_t state)
{
    struct profile_bucket *bucket = &buckets[cpu][scope];
    uint64_t epoch = state >> 32;
    if (__atomic_load_n(&bucket->epoch, __ATOMIC_ACQUIRE) != epoch) {
        clear_values(&bucket->value);
        __atomic_store_n(&bucket->epoch, epoch, __ATOMIC_RELEASE);
    }
    return &bucket->value;
}

/** @brief Check the runtime switch before requesting a profiling CPU index.
 * @return True when the current session is enabled. Atomic load only; callable
 * with IRQs masked, without allocation, timestamp reads or counter writes. */
bool lock_profile_enabled(void)
{
    return (__atomic_load_n(&profile_state, __ATOMIC_ACQUIRE) & 1) != 0;
}

/** @brief Start a possible outer acquisition without touching counters when off.
 * @param sample Stack-owned sample; initialized even for an invalid/off entry.
 * @param cpu Physical index; invalid values are dropped.
 * @param scope Fixed ID; invalid values are dropped.
 * IRQs already masked; no lock, allocation or scheduling. */
void lock_profile_begin(struct lock_profile_sample *sample, uint32_t cpu, uint32_t scope)
{
    sample->state = 0;
    if (cpu >= SMP_MAX_CPUS || scope >= LOCK_SCOPE_COUNT) return;
    uint64_t state = __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    if (!(state & 1)) return;
    sample->cpu = cpu; sample->scope = scope; sample->state = state;
    sample->start = profile_clock();
}

/** @brief Commit one successful outer admission and remember its hold segment.
 * @param sample Required borrowed earlier begin result.
 * @param contended True if admission spun.
 * Local IRQs remain masked and prevent same-CPU ownership migration. */
void lock_profile_acquired(const struct lock_profile_sample *sample, bool contended)
{
    uint64_t state = sample->state;
    if (!state || __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE) != state) return;
    uint64_t now = profile_clock();
    struct lock_profile_stats *value = bucket_for(sample->cpu, sample->scope, state);
    add_sat(&value->acquisitions, 1);
    if (contended) add_sat(&value->contentions, 1);
    if (now >= sample->start) {
        uint64_t wait = now - sample->start;
        add_sat(&value->wait_total, wait); max_u64(&value->wait_max, wait);
        add_sat(&value->wait_hist[bucket_index(wait)], 1);
    } else add_sat(&value->clock_errors, 1);
    active[sample->cpu] = (struct profile_active){state, sample->start, now, sample->scope};
}

/** @brief Finish a successful outer segment before IRQ restoration.
 * @param cpu Current physical CPU; invalid indexes are ignored.
 * Reset/disable invalidates old segments without a clock read or record write. */
void lock_profile_release(uint32_t cpu)
{
    if (cpu >= SMP_MAX_CPUS) return;
    uint64_t state = __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    if (!(state & 1) || active[cpu].state != state) return;
    struct profile_active entry = active[cpu];
    uint64_t now = profile_clock();
    struct lock_profile_stats *value = bucket_for(cpu, entry.scope, state);
    if (now >= entry.acquired) {
        uint64_t hold = now - entry.acquired;
        add_sat(&value->hold_total, hold); max_u64(&value->hold_max, hold);
        add_sat(&value->hold_hist[bucket_index(hold)], 1);
    } else add_sat(&value->clock_errors, 1);
    if (now >= entry.start) {
        uint64_t irq = now - entry.start;
        add_sat(&value->irq_total, irq); max_u64(&value->irq_max, irq);
        add_sat(&value->irq_hist[bucket_index(irq)], 1);
    } else add_sat(&value->clock_errors, 1);
    active[cpu].state = 0;
}

/** @brief Apply one complete diagnostic control value.
 * @param command Required validated readable bytes, not necessarily terminated.
 * @param length Exact length including an optional LF.
 * @return Zero or negative EINVAL/EOVERFLOW; failures leave state unchanged.
 * Caller serializes control operations; no clock, wait, allocation or logging. */
int lock_profile_control(const char *command, uint32_t length)
{
    if (!command || !length) return -EINVAL;
    if (command[length - 1] == '\n') --length;
    uint64_t state = __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    uint64_t epoch = state >> 32, session = (state >> 1) & 0x7fffffffULL;
    bool enabled = (state & 1) != 0;
    bool reset = length == 5 && !__builtin_memcmp(command, "reset", 5);
    if (length == 1 && (command[0] == '0' || command[0] == '1')) {
        bool requested = command[0] == '1';
        if (enabled == requested) return 0;
        enabled = requested;
    } else if (!reset) return -EINVAL;
    if (session == 0x7fffffffULL) {
        /* Exhaustion is fail-closed: an enabled sampler can still be stopped. */
        if (!reset && !enabled) {
            __atomic_store_n(&profile_state, state & ~1ULL, __ATOMIC_RELEASE);
            return 0;
        }
        return -EOVERFLOW;
    }
    if (reset && epoch == UINT32_MAX) return -EOVERFLOW;
    if (reset) ++epoch;
    ++session;
    __atomic_store_n(&profile_state, (epoch << 32) | (session << 1) | enabled, __ATOMIC_RELEASE);
    return 0;
}

/** @brief Atomically copy one CPU/scope's current-window counters.
 * @param cpu Physical index below SMP_MAX_CPUS.
 * @param scope Fixed ID below LOCK_SCOPE_COUNT.
 * @param out Required caller-owned writable output.
 * @return Zero or negative EINVAL. Concurrent updates are a best-effort view;
 * atomic reads require no lock or allocation and do not transfer ownership. */
int lock_profile_snapshot(uint32_t cpu, uint32_t scope, struct lock_profile_stats *out)
{
    if (!out || cpu >= SMP_MAX_CPUS || scope >= LOCK_SCOPE_COUNT) return -EINVAL;
    *out = (struct lock_profile_stats){0};
    uint64_t state = __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    struct profile_bucket *bucket = &buckets[cpu][scope];
    if (__atomic_load_n(&bucket->epoch, __ATOMIC_ACQUIRE) != state >> 32) return 0;
    copy_values(out, &bucket->value);
    if ((__atomic_load_n(&profile_state, __ATOMIC_ACQUIRE) >> 32) != state >> 32 ||
        __atomic_load_n(&bucket->epoch, __ATOMIC_ACQUIRE) != state >> 32)
        *out = (struct lock_profile_stats){0};
    return 0;
}

/** @brief Classify syscall numbers into fixed, bounded operation groups.
 * @param number Linux syscall number from the saved entry frame.
 * @return One documented scope. Pure IRQ-safe classification; no pointers,
 * mutable state, allocation or ownership transfer. */
uint32_t lock_profile_syscall_scope(uint64_t number)
{
    switch (number) {
    case LINUX_SYS_READ: case LINUX_SYS_WRITE: case LINUX_SYS_PREAD64: case LINUX_SYS_PWRITE64:
    case LINUX_SYS_READV: case LINUX_SYS_WRITEV: case LINUX_SYS_PREADV: case LINUX_SYS_PWRITEV:
    case LINUX_SYS_PREADV2: case LINUX_SYS_PWRITEV2: case LINUX_SYS_SENDFILE: case LINUX_SYS_COPY_FILE_RANGE:
    case LINUX_SYS_OPEN: case LINUX_SYS_OPENAT: case LINUX_SYS_OPENAT2: case LINUX_SYS_CREAT:
    case LINUX_SYS_CLOSE: case LINUX_SYS_CLOSE_RANGE: case LINUX_SYS_LSEEK:
    case LINUX_SYS_STAT: case LINUX_SYS_LSTAT: case LINUX_SYS_FSTAT: case LINUX_SYS_NEWFSTATAT:
    case LINUX_SYS_STATX: case LINUX_SYS_STATFS: case LINUX_SYS_FSTATFS:
    case LINUX_SYS_GETDENTS: case LINUX_SYS_GETDENTS64: case LINUX_SYS_ACCESS:
    case LINUX_SYS_FACCESSAT: case LINUX_SYS_FACCESSAT2: case LINUX_SYS_IOCTL:
    case LINUX_SYS_FCNTL: case LINUX_SYS_FLOCK: case LINUX_SYS_DUP: case LINUX_SYS_DUP2: case LINUX_SYS_DUP3:
    case LINUX_SYS_FTRUNCATE: case LINUX_SYS_TRUNCATE: case LINUX_SYS_FALLOCATE:
    case LINUX_SYS_FSYNC: case LINUX_SYS_FDATASYNC: case LINUX_SYS_SYNC: case LINUX_SYS_SYNCFS:
    case LINUX_SYS_SYNC_FILE_RANGE: case LINUX_SYS_READAHEAD: case LINUX_SYS_FADVISE64:
    case LINUX_SYS_CHDIR: case LINUX_SYS_FCHDIR: case LINUX_SYS_GETCWD:
    case LINUX_SYS_MKDIR: case LINUX_SYS_MKDIRAT: case LINUX_SYS_RMDIR:
    case LINUX_SYS_RENAME: case LINUX_SYS_RENAMEAT: case LINUX_SYS_RENAMEAT2:
    case LINUX_SYS_UNLINK: case LINUX_SYS_UNLINKAT: case LINUX_SYS_LINK: case LINUX_SYS_LINKAT:
    case LINUX_SYS_SYMLINK: case LINUX_SYS_SYMLINKAT: case LINUX_SYS_READLINK: case LINUX_SYS_READLINKAT:
    case LINUX_SYS_CHMOD: case LINUX_SYS_FCHMOD: case LINUX_SYS_FCHMODAT: case LINUX_SYS_FCHMODAT2:
    case LINUX_SYS_CHOWN: case LINUX_SYS_FCHOWN: case LINUX_SYS_LCHOWN: case LINUX_SYS_FCHOWNAT:
    case LINUX_SYS_UTIME: case LINUX_SYS_UTIMES: case LINUX_SYS_FUTIMESAT: case LINUX_SYS_UTIMENSAT:
    case LINUX_SYS_MOUNT: case LINUX_SYS_UMOUNT2:
        return LOCK_SCOPE_SYSCALL_IO;
    case LINUX_SYS_CLONE: case LINUX_SYS_CLONE3: case LINUX_SYS_FORK: case LINUX_SYS_VFORK:
    case LINUX_SYS_EXECVE: case LINUX_SYS_EXECVEAT: case LINUX_SYS_EXIT: case LINUX_SYS_EXIT_GROUP:
    case LINUX_SYS_WAIT4: case LINUX_SYS_WAITID: case LINUX_SYS_FUTEX: case LINUX_SYS_FUTEX_WAIT:
    case LINUX_SYS_FUTEX_WAKE: case LINUX_SYS_FUTEX_REQUEUE: case LINUX_SYS_FUTEX_WAITV:
        return LOCK_SCOPE_SYSCALL_PROCESS;
    case LINUX_SYS_MMAP: case LINUX_SYS_MUNMAP: case LINUX_SYS_MREMAP: case LINUX_SYS_MPROTECT:
    case LINUX_SYS_BRK: case LINUX_SYS_MADVISE: case LINUX_SYS_MSYNC: case LINUX_SYS_MINCORE:
    case LINUX_SYS_MLOCK: case LINUX_SYS_MUNLOCK: case LINUX_SYS_MLOCKALL: case LINUX_SYS_MUNLOCKALL:
    case LINUX_SYS_MLOCK2: case LINUX_SYS_MEMFD_CREATE:
        return LOCK_SCOPE_SYSCALL_MEMORY;
    default: return LOCK_SCOPE_SYSCALL_OTHER;
    }
}

/** @brief Emit all histogram counts to an offset-aware bounded stream.
 * @param stream Required caller-owned writable text stream.
 * @param values Required borrowed array of LOCK_PROFILE_BINS counts.
 * Task context or an already synchronized snapshot; no allocation or I/O. */
static void emit_hist(struct text_stream *stream, const uint64_t *values)
{
    for (uint32_t i = 0; i < LOCK_PROFILE_BINS; ++i) {
        if (i) text_string(stream, ",");
        text_unsigned(stream, values[i]);
    }
}

struct profile_view {
    uint64_t state;
    uint32_t cpus;
    struct lock_profile_stats stats[];
};

/** @brief Capture bounded per-CPU counters for one proc open file description.
 * @param out Required writable output; receives an owned view or NULL on error.
 * @return Zero or negative ENOMEM/EINVAL. Task context under execution
 * ownership; open may allocate but hot sampling hooks do not. */
int lock_profile_view_new(void **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    uint32_t cpus = smp_cpu_count(); if (cpus > SMP_MAX_CPUS) cpus = SMP_MAX_CPUS;
    size_t count = (size_t)cpus * LOCK_SCOPE_COUNT;
    struct profile_view *view = kernel_malloc(sizeof(*view) + count * sizeof(view->stats[0]));
    if (!view) return -ENOMEM;
    view->state = __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    view->cpus = cpus;
    for (uint32_t cpu = 0; cpu < cpus; ++cpu)
        for (uint32_t scope = 0; scope < LOCK_SCOPE_COUNT; ++scope)
            lock_profile_snapshot(cpu, scope, &view->stats[cpu * LOCK_SCOPE_COUNT + scope]);
    *out = view;
    return 0;
}

/** @brief Render a captured or live ReliefNT diagnostic as an offset-aware stream.
 * @param view Optional borrowed immutable capture, NULL for a live view.
 * @param offset File byte offset.
 * @param buffer Writable validated destination, NULL only at zero capacity.
 * @param capacity Available bytes.
 * @param read Required writable output byte count.
 * @return Zero or negative EINVAL. Task context under execution ownership;
 * captured views remain caller-owned and the live view may change. */
static int profile_render(const struct profile_view *view, uint64_t offset,
                          void *buffer, uint32_t capacity, uint32_t *read)
{
    if (!read || (!buffer && capacity)) return -EINVAL;
    *read = 0;
    if (!capacity) return 0;
    struct text_stream stream = {.offset = offset, .buffer = buffer, .capacity = capacity};
    static const char *names[] = {"generic", "syscall_io", "syscall_process", "syscall_memory",
                                  "syscall_other", "fault", "sched", "resumed"};
    uint64_t state = view ? view->state : __atomic_load_n(&profile_state, __ATOMIC_ACQUIRE);
    text_string(&stream, "reliefnt_lock_stats 1 unit=tsc_cycles enabled="); text_unsigned(&stream, state & 1);
    text_string(&stream, " epoch="); text_unsigned(&stream, state >> 32);
    text_string(&stream, " histogram=log2_cycles irq=gate_masked_window\n");
    if (stream.written == capacity) { *read = stream.written; return 0; }
    uint32_t cpus = view ? view->cpus : smp_cpu_count(); if (cpus > SMP_MAX_CPUS) cpus = SMP_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < cpus; ++cpu) for (uint32_t scope = 0; scope < LOCK_SCOPE_COUNT; ++scope) {
        struct lock_profile_stats s;
        if (view) s = view->stats[cpu * LOCK_SCOPE_COUNT + scope];
        else lock_profile_snapshot(cpu, scope, &s);
        if (!s.acquisitions) continue;
        text_string(&stream, "cpu="); text_unsigned(&stream, cpu);
        text_string(&stream, " scope="); text_string(&stream, names[scope]);
#define FIELD(name) text_string(&stream, " " #name "="); text_unsigned(&stream, s.name)
        FIELD(acquisitions); FIELD(contentions); FIELD(wait_total); FIELD(wait_max);
        FIELD(hold_total); FIELD(hold_max); FIELD(irq_total); FIELD(irq_max); FIELD(clock_errors);
#undef FIELD
        text_string(&stream, " wait_hist="); emit_hist(&stream, s.wait_hist);
        text_string(&stream, " hold_hist="); emit_hist(&stream, s.hold_hist);
        text_string(&stream, " irq_hist="); emit_hist(&stream, s.irq_hist);
        text_string(&stream, "\n");
        if (stream.written == capacity) { *read = stream.written; return 0; }
    }
    *read = stream.written;
    return 0;
}

/** @brief Render current counters for an internal offset-aware direct read.
 * @param offset Starting byte position.
 * @param buffer Writable validated buffer, nullable only at zero capacity.
 * @param capacity Maximum output bytes.
 * @param read Required writable output count.
 * @return Zero or negative EINVAL.
 * Caller holds execution ownership; successive live reads can differ. */
int lock_profile_read(uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read)
{ return profile_render(NULL, offset, buffer, capacity, read); }
/** @brief Render an open-file snapshot at a byte offset.
 * @param view Required borrowed snapshot from lock_profile_view_new.
 * @param offset Starting byte position.
 * @param buffer Writable validated buffer, nullable only at zero capacity.
 * @param capacity Maximum output bytes.
 * @param read Required writable output count.
 * @return Zero or negative EINVAL.
 * Caller retains the view and execution ownership; no allocation or I/O. */
int lock_profile_view_read(void *view, uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read)
{ return view ? profile_render(view, offset, buffer, capacity, read) : -EINVAL; }
/** @brief Release the allocation owned by one open-file snapshot.
 * @param view Owned optional snapshot, NULL accepted. Task context under
 * execution ownership; caller must not use the consumed view afterward. */
void lock_profile_view_free(void *view) { kernel_free(view); }

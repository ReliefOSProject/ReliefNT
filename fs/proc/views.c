/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/proc_stats.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <reliefnt/mm.h>
#include <reliefnt/exec_file.h>
#include <reliefnt/page_cache.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/time.h>

static const char *net_leaves[] = {"dev", "route", "arp", "tcp", "udp", "unix"};
static const char *fs_leaves[] = {"nr_open",
                                  "file-nr",
                                  "file-max",
                                  "pipe-max-size",
                                  "pipe-user-pages-soft",
                                  "pipe-user-pages-hard"};
static const char *task_leaves[] = {"status",       "statm",  "maps", "smaps",
                                    "smaps_rollup", "limits", "auxv", "io"};

/** @brief Check an exact borrowed name against a constant table.
 * @param name Nullable name. @param names Table. @param count Table length.
 * @return Membership; no allocation, references or state changes.
 */
static bool member(const char *name, const char *const *names, size_t count)
{
    if (!name)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (!__builtin_strcmp(name, names[i]))
            return true;
    return false;
}
/** @brief Parse an fdinfo descriptor without signs, overflow or trailing bytes.
 * @param leaf Borrowed task-relative key. @return Descriptor or -1.
 */
static int fdinfo_number(const char *leaf)
{
    if (!leaf || __builtin_strncmp(leaf, "fdinfo/", 7) || !leaf[7])
        return -1;
    uint32_t value = 0;
    for (const char *p = leaf + 7; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (SCHED_NR_OPEN - 1u - (uint32_t)(*p - '0')) / 10u)
            return -1;
        value = value * 10u + *p - '0';
    }
    return value < SCHED_NR_OPEN ? (int)value : -1;
}
/** @brief Borrow a live ordinary/PTY descriptor for metadata, preserving aliases.
 * @param t Live task. @param fd Descriptor. @param pty Required nullable-endpoint output.
 * @return Canonical OFD or NULL; endpoint output may independently be nonnull.
 */
static struct task_file *view_file(struct task *t, int fd, struct task_pty_fd **pty)
{
    *pty = task_pty_fd_for_fd(t, fd);
    return task_file_for_fd(t, fd);
}
/** @brief Identify added nodes and replacements using exact intrinsic keys.
 * @param path Borrowed key. @return File=1, directory=2, link=3, unknown=0.
 * Execution-owned; absent tasks or descriptors do not become phantom nodes.
 */
int proc_extra_kind(const char *path)
{
    if (!path)
        return 0;
    if (!__builtin_strcmp(path, "/proc/self") || !__builtin_strcmp(path, "/proc/thread-self"))
        return sched_current_task() ? 3 : 0;
    if (!__builtin_strcmp(path, "/proc/net") || !__builtin_strcmp(path, "/proc/sys/fs"))
        return 2;
    if (!__builtin_strncmp(path, "/proc/net/", 10))
        return member(path + 10, net_leaves, sizeof(net_leaves) / sizeof(*net_leaves));
    if (!__builtin_strncmp(path, "/proc/sys/fs/", 13))
        return member(path + 13, fs_leaves, sizeof(fs_leaves) / sizeof(*fs_leaves));
    static const char *system[] = {
        "/proc/loadavg",    "/proc/meminfo",   "/proc/stat",
        "/proc/partitions", "/proc/diskstats", "/proc/interrupts",
        "/proc/vmstat",     "/proc/swaps",     "/proc/sys/kernel/pid_max"};
    if (member(path, system, sizeof(system) / sizeof(*system)))
        return 1;
    const char *leaf = NULL;
    struct task *t = proc_view_task(path, &leaf);
    if (!t || !leaf)
        return 0;
    if (!__builtin_strcmp(leaf, "task") || !__builtin_strcmp(leaf, "fdinfo"))
        return 2;
    if (member(leaf, task_leaves, sizeof(task_leaves) / sizeof(*task_leaves)))
        return 1;
    int fd = fdinfo_number(leaf);
    struct task_pty_fd *pty = NULL;
    if (fd >= 0 && (view_file(t, fd, &pty) || (pty && pty->used)))
        return 1;
    return 0;
}
/** @brief Emit a label, integer and unit as one complete proc field.
 * @param s Required stream. @param label Prefix. @param n Value. @param unit Suffix.
 */
static void field(struct text_stream *s, const char *label, uint64_t n, const char *unit)
{
    text_string(s, label);
    text_unsigned(s, n);
    text_string(s, unit);
}
/** @brief Render a bounded octal integer without libc or scratch allocation.
 * @param s Stream. @param n Native flags/mode value.
 */
static void octal(struct text_stream *s, uint32_t n)
{
    char digits[11];
    unsigned used = 0;
    do {
        digits[used++] = '0' + (n & 7);
        n >>= 3;
    } while (n);
    text_string(s, "0");
    while (used)
        text_bytes(s, &digits[--used], 1);
}
/** @brief Emit hex with a minimum width, preserving large offsets/device IDs.
 * @param s Required output. @param value Scalar. @param minimum Minimum digits.
 */
static void hex_min(struct text_stream *s, uint64_t value, unsigned minimum)
{
    unsigned digits = 1;
    for (uint64_t high = value >> 4; high; high >>= 4)
        ++digits;
    text_hex(s, value, digits > minimum ? digits : minimum);
}
/** @brief Emit a name with Linux proc newline/backslash escaping.
 * @param s Output. @param name Borrowed string. @param length Maximum bytes.
 */
static void escaped(struct text_stream *s, const char *name, size_t length)
{
    for (size_t i = 0; name && name[i] && i < length; ++i) {
        if (name[i] == '\n')
            text_string(s, "\\012");
        else if (name[i] == '\\')
            text_string(s, "\\134");
        else
            text_bytes(s, name + i, 1);
    }
}
/** @brief Count live threads in one thread group without a fixed table bound.
 * @param t Borrowed task. @return Number of published group members.
 */
static uint32_t threads(struct task *t)
{
    uint32_t n = 0;
    struct task *o;
    for (uint32_t i = 0; (o = sched_task_at(i)); ++i)
        if (o->pid && sched_task_tgid(o) == sched_task_tgid(t))
            ++n;
    return n;
}

struct memory_totals {
    uint64_t size, rss, shared, text, data, stack, locked, pss, referenced, private_clean,
        private_dirty, shared_clean, shared_dirty, anonymous;
};
/** @brief Count virtual layout bytes without faulting or populating pages.
 * @param t Live task. @param out Required zeroed output.
 */
static void virtual_totals(struct task *t, struct memory_totals *out)
{
    struct task_address_space_state *mm = sched_task_mm(t);
    for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX + mm->vma_extra_count; ++i) {
        struct task_vma *v = sched_task_vma_at(t, i);
        if (!v || !v->used)
            continue;
        uint64_t bytes = v->end - v->start;
        out->size += bytes;
        if (v->prot & TASK_VMA_PROT_EXEC)
            out->text += bytes;
        bool stack = v->start < t->stack_top && v->end > t->stack_low;
        if (stack)
            out->stack += bytes;
        else if ((v->prot & TASK_VMA_PROT_WRITE) && !(v->flags & TASK_VMA_FLAG_SHARED))
            out->data += bytes;
        if (v->flags & TASK_VMA_FLAG_LOCKED)
            out->locked += bytes;
    }
    out->rss = address_space_user_resident_kib(&mm->as) * 1024;
}
/** @brief Render actual credential, signal, affinity and memory state.
 * @param t Borrowed live task. @param s Required output, execution-owned.
 */
static void status(struct task *t, struct text_stream *s)
{
    text_string(s, "Name:\t");
    escaped(s, t->name ? t->name : "?", 15);
    const char *state = t->state == TASK_EXITED    ? "Z (zombie)"
                        : t->state == TASK_STOPPED ? "T (stopped)"
                        : t->state == TASK_BLOCKED ? "S (sleeping)"
                                                   : "R (running)";
    text_string(s, "\nState:\t");
    text_string(s, state);
    field(s, "\nTgid:\t", sched_task_tgid(t), "\n");
    field(s, "Pid:\t", t->pid, "\n");
    field(s, "PPid:\t", t->parent_pid, "\n");
    uint32_t ids[] = {t->uid, t->euid, t->suid, t->fsuid, t->gid, t->egid, t->sgid, t->fsgid};
    for (unsigned i = 0; i < 8; ++i) {
        text_string(s, i == 0 ? "Uid:\t" : i == 4 ? "\nGid:\t" : "\t");
        text_unsigned(s, ids[i]);
    }
    text_string(s, "\nGroups:\t");
    if (t->groups)
        for (uint32_t i = 0; i < t->groups->count; ++i) {
            text_unsigned(s, t->groups->ids[i]);
            text_string(s, " ");
        }
    text_string(s, "\nUmask:\t");
    uint32_t mask = *sched_task_umask(t) & 0777u;
    char umask[4] = {'0', (char)('0' + ((mask >> 6) & 7)), (char)('0' + ((mask >> 3) & 7)),
                     (char)('0' + (mask & 7))};
    text_bytes(s, umask, 4);
    text_string(s, "\n");
    struct memory_totals m = {0};
    virtual_totals(t, &m);
    field(s, "VmSize:\t", m.size / 1024, " kB\n");
    field(s, "VmRSS:\t", m.rss / 1024, " kB\n");
    field(s, "VmData:\t", m.data / 1024, " kB\n");
    field(s, "VmStk:\t", m.stack / 1024, " kB\n");
    field(s, "VmExe:\t", m.text / 1024, " kB\n");
    field(s, "VmLck:\t", m.locked / 1024, " kB\n");
    field(s, "Threads:\t", threads(t), "\n");
    uint64_t ignored = 0, caught = 0;
    struct kernel_signal_action *actions = sched_task_actions(t);
    for (unsigned sig = 1; sig < KERNEL_SIGNAL_ACTION_MAX; ++sig) {
        if (actions[sig].handler == 1)
            ignored |= 1ULL << (sig - 1);
        else if (actions[sig].handler > 1)
            caught |= 1ULL << (sig - 1);
    }
    const char *labels[] = {"SigPnd:\t", "ShdPnd:\t", "SigBlk:\t", "SigIgn:\t", "SigCgt:\t",
                            "CapInh:\t", "CapPrm:\t", "CapEff:\t", "CapBnd:\t", "CapAmb:\t"};
    uint64_t masks[] = {t->pending_signals | t->timer_pending_signals,
                        *sched_task_process_pending(t),
                        t->blocked_signals,
                        ignored,
                        caught,
                        t->cap_inheritable,
                        t->cap_permitted,
                        t->cap_effective,
                        t->cap_bset,
                        t->cap_ambient};
    for (unsigned i = 0; i < 10; ++i) {
        text_string(s, labels[i]);
        text_hex(s, masks[i], 16);
        text_string(s, "\n");
    }
    field(s, "NoNewPrivs:\t", t->no_new_privs, "\n");
    uint32_t cpus = smp_cpu_count();
    if (cpus > 64)
        cpus = 64;
    uint64_t allowed =
        t->affinity_mask ? t->affinity_mask : (cpus == 64 ? UINT64_MAX : (1ULL << cpus) - 1);
    text_string(s, "Cpus_allowed:\t");
    text_hex(s, allowed >> 32, 8);
    text_string(s, ",");
    text_hex(s, allowed, 8);
    text_string(s, "\nCpus_allowed_list:\t");
    bool first = true;
    for (unsigned c = 0; c < 64; ++c)
        if (allowed & (1ULL << c)) {
            if (!first)
                text_string(s, ",");
            text_unsigned(s, c);
            first = false;
        }
    text_string(s, "\n");
    proc_task_identity_fields(t, s);
}
/** @brief Emit the seven resource limits actually enforced/stored by the kernel.
 * @param t Live target. @param s Required stream; query never changes limits.
 */
static void limits(struct task *t, struct text_stream *s)
{
    struct task_rlimit_state *l = sched_task_limits(t);
    const char *names[] = {"Max open files",   "Max address space", "Max pending signals",
                           "Max stack size",   "Max processes",     "Max core file size",
                           "Max locked memory"};
    const char *units[] = {"files", "bytes", "signals", "bytes", "processes", "bytes", "bytes"};
    struct linux_rlimit64 *values[] = {&l->nofile, &l->as,   &l->sigpending, &l->stack,
                                       &l->nproc,  &l->core, &l->memlock};
    text_string(s, "Limit                     Soft Limit           Hard Limit           Units\n");
    for (unsigned i = 0; i < 7; ++i) {
        text_string(s, names[i]);
        for (size_t p = __builtin_strlen(names[i]); p < 26; ++p)
            text_string(s, " ");
        for (unsigned j = 0; j < 2; ++j) {
            uint64_t n = j ? values[i]->rlim_max : values[i]->rlim_cur;
            uint64_t before = s->position;
            if (n == UINT64_MAX)
                text_string(s, "unlimited");
            else
                text_unsigned(s, n);
            uint64_t used = s->position - before;
            while (used++ < 21)
                text_string(s, " ");
        }
        text_string(s, units[i]);
        text_string(s, "\n");
    }
}

struct page_count {
    uint64_t phys, count;
};
struct map_counts {
    struct page_count *entries;
    size_t capacity;
};
/** @brief Find an existing physical-page map count, without reference acquisition.
 * @param counts Snapshot hash. @param phys Page base. @return Slot, empty on miss.
 */
static struct page_count *page_slot(struct map_counts *counts, uint64_t phys)
{
    size_t i = ((phys >> 12) * 11400714819323198485ULL) & (counts->capacity - 1);
    for (size_t n = 0; n < counts->capacity; ++n, i = (i + 1) & (counts->capacity - 1))
        if (!counts->entries[i].phys || counts->entries[i].phys == phys)
            return &counts->entries[i];
    return NULL;
}
/** @brief Ignore duplicate CLONE_VM task views when counting mappings.
 * @param index Task slot. @param mm Address space. @return First task using mm.
 */
static bool first_mm(uint32_t index, struct task_address_space_state *mm)
{
    for (uint32_t i = 0; i < index; ++i)
        if (sched_task_mm(sched_task_at(i)) == mm)
            return false;
    return true;
}
/** @brief Build exact user PTE mapping counts, excluding kernel/cache references.
 * @param counts Required empty output. @return Zero or ENOMEM, execution-owned.
 * Traverses allocated page-table branches; sparse virtual holes are skipped.
 */
static int build_map_counts(struct map_counts *counts)
{
    size_t pages = 0;
    struct task *t;
    for (uint32_t i = 0; (t = sched_task_at(i)); ++i)
        if (first_mm(i, sched_task_mm(t))) {
            uint64_t n = sched_task_as(t)->user_page_count;
            if (n > SIZE_MAX / sizeof(struct page_count) / 4 - pages)
                return -ENOMEM;
            pages += (size_t)n;
        }
    size_t capacity = 16;
    while (capacity < pages * 2) {
        if (capacity > SIZE_MAX / 2 / sizeof(struct page_count))
            return -ENOMEM;
        capacity *= 2;
    }
    counts->entries = kernel_malloc(capacity * sizeof(*counts->entries));
    if (!counts->entries)
        return -ENOMEM;
    counts->capacity = capacity;
    __builtin_memset(counts->entries, 0, capacity * sizeof(*counts->entries));
    for (uint32_t i = 0; (t = sched_task_at(i)); ++i)
        if (first_mm(i, sched_task_mm(t))) {
            uint64_t at = RELIEFNT_USER_BASE, page, phys, flags;
            while (address_space_user_page_next(sched_task_as(t), at, RELIEFNT_USER_TOP, &page,
                                                &phys, &flags)) {
                at = page + 4096;
                if (!phys || (flags & RELIEFNT_PAGE_DEVICE))
                    continue;
                struct page_count *entry = page_slot(counts, phys);
                if (!entry) {
                    kernel_free(counts->entries);
                    counts->entries = NULL;
                    return -ENOMEM;
                }
                entry->phys = phys;
                ++entry->count;
            }
        }
    return 0;
}
/** @brief Account present backing in a VMA using real PTE flags and map counts.
 * @param t Task. @param v VMA. @param counts Map-count snapshot. @param m Output.
 */
static void region_memory(struct task *t, const struct task_vma *v, struct map_counts *counts,
                          struct memory_totals *m)
{
    m->size = v->end - v->start;
    uint64_t at = v->start, page, phys, flags;
    while (address_space_user_page_next(sched_task_as(t), at, v->end, &page, &phys, &flags)) {
        at = page + 4096;
        if (flags & RELIEFNT_PAGE_DEVICE)
            continue;
        struct page_count *entry = page_slot(counts, phys);
        uint64_t n = entry ? entry->count : 1;
        if (!n)
            n = 1;
        m->rss += 4096;
        m->pss += 4096 * 1024 / n;
        if (flags & 0x20)
            m->referenced += 4096;
        bool dirty = flags & 0x40;
        if (n > 1) {
            m->shared += 4096;
            if (dirty)
                m->shared_dirty += 4096;
            else
                m->shared_clean += 4096;
        } else if (dirty)
            m->private_dirty += 4096;
        else
            m->private_clean += 4096;
        if ((v->flags & TASK_VMA_FLAG_ANON) ||
            (!(v->flags & TASK_VMA_FLAG_SHARED) && !page_cache_owns(phys)))
            m->anonymous += 4096;
        if (v->flags & TASK_VMA_FLAG_LOCKED)
            m->locked += 4096;
    }
}
/** @brief Render maps header from owned backing, preserving deleted object names.
 * @param t Target. @param v VMA. @param s Output. @return Zero or metadata errno.
 */
static int map_header(struct task *t, const struct task_vma *v, struct text_stream *s)
{
    text_hex(s, v->start, 16);
    text_string(s, "-");
    text_hex(s, v->end, 16);
    text_string(s, " ");
    char perms[] = {(v->prot & 1) ? 'r' : '-', (v->prot & 2) ? 'w' : '-', (v->prot & 4) ? 'x' : '-',
                    (v->flags & TASK_VMA_FLAG_SHARED) ? 's' : 'p', ' '};
    text_bytes(s, perms, 5);
    hex_min(s, v->file_offset, 8);
    text_string(s, " ");
    const struct vfs_path *path =
        v->mapping_file ? &v->mapping_file->path_ref : vfs_exec_source_path(v->exec_source);
    struct linux_stat_abi st = {0};
    if (path && path->mount) {
        int rc = vfs_path_stat(path, &st);
        if (rc)
            return rc;
    }
    hex_min(s, (st.st_dev >> 8) & 4095u, 2);
    text_string(s, ":");
    hex_min(s, (st.st_dev & 255u) | ((st.st_dev >> 12) & 0xfffff00u), 2);
    text_string(s, " ");
    text_unsigned(s, st.st_ino);
    if (path && path->mount) {
        char *name = kernel_malloc(VFS_PATH_MAX + sizeof(" (deleted)"));
        if (!name)
            return -ENOMEM;
        int rc = proc_object_target(path, name, VFS_PATH_MAX + sizeof(" (deleted)"));
        if (!rc) {
            text_string(s, " ");
            escaped(s, name, SIZE_MAX);
        }
        kernel_free(name);
        if (rc)
            return rc;
    } else {
        const char *label = (v->flags & TASK_VMA_FLAG_VDSO)                    ? "[vdso]"
                            : (v->flags & TASK_VMA_FLAG_VVAR)                  ? "[vvar]"
                            : v->start < t->stack_top && v->end > t->stack_low ? "[stack]"
                            : sched_task_mm(t)->program_break_base >= v->start &&
                                    sched_task_mm(t)->program_break_base < v->end
                                ? "[heap]"
                                : NULL;
        if (label) {
            text_string(s, " ");
            text_string(s, label);
        }
    }
    text_string(s, "\n");
    return 0;
}
/** @brief Emit measured smaps fields; unsupported swap/huge pages remain zero.
 * @param m Actual accounting. @param s Required output.
 */
static void smaps_fields(struct memory_totals *m, struct text_stream *s)
{
    field(s, "Size: ", m->size / 1024, " kB\n");
    text_string(s, "KernelPageSize: 4 kB\nMMUPageSize: 4 kB\n");
    field(s, "Rss: ", m->rss / 1024, " kB\n");
    field(s, "Pss: ", m->pss / (1024 * 1024), " kB\n");
    field(s, "Shared_Clean: ", m->shared_clean / 1024, " kB\n");
    field(s, "Shared_Dirty: ", m->shared_dirty / 1024, " kB\n");
    field(s, "Private_Clean: ", m->private_clean / 1024, " kB\n");
    field(s, "Private_Dirty: ", m->private_dirty / 1024, " kB\n");
    field(s, "Referenced: ", m->referenced / 1024, " kB\n");
    field(s, "Anonymous: ", m->anonymous / 1024, " kB\n");
    text_string(s, "AnonHugePages: 0 kB\nSwap: 0 kB\nSwapPss: 0 kB\n");
    field(s, "Locked: ", m->locked / 1024, " kB\n");
}
/** @brief Emit address-sorted mapping metadata, accounting only backed leaves.
 * @param t Target. @param leaf Maps/smaps selection. @param s Required stream.
 * @return Zero or allocation/metadata errno. All temporary snapshots are freed.
 */
static int maps(struct task *t, const char *leaf, struct text_stream *s)
{
    bool detailed = __builtin_strcmp(leaf, "maps") != 0,
         rollup = !__builtin_strcmp(leaf, "smaps_rollup");
    struct map_counts counts = {0};
    if (detailed) {
        int rc = build_map_counts(&counts);
        if (rc)
            return rc;
    }
    struct task_address_space_state *mm = sched_task_mm(t);
    struct memory_totals total = {0};
    uint64_t cursor = 0, first = UINT64_MAX, last = 0;
    int rc = 0;
    for (;;) {
        struct task_vma *v = NULL;
        for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX + mm->vma_extra_count; ++i) {
            struct task_vma *p = sched_task_vma_at(t, i);
            if (p && p->used && p->end > p->start && p->start >= cursor &&
                (!v || p->start < v->start))
                v = p;
        }
        if (!v)
            break;
        cursor = v->end;
        if (v->start < first)
            first = v->start;
        last = v->end;
        if (!rollup && (rc = map_header(t, v, s)))
            break;
        if (detailed) {
            struct memory_totals m = {0};
            region_memory(t, v, &counts, &m);
            if (rollup) {
                total.size += m.size;
                total.rss += m.rss;
                total.pss += m.pss;
                total.shared_clean += m.shared_clean;
                total.shared_dirty += m.shared_dirty;
                total.private_clean += m.private_clean;
                total.private_dirty += m.private_dirty;
                total.referenced += m.referenced;
                total.anonymous += m.anonymous;
                total.locked += m.locked;
            } else {
                smaps_fields(&m, s);
                text_string(s, "VmFlags:");
                if (v->prot & 1)
                    text_string(s, " rd");
                if (v->prot & 2)
                    text_string(s, " wr");
                if (v->prot & 4)
                    text_string(s, " ex");
                if (v->flags & TASK_VMA_FLAG_SHARED)
                    text_string(s, " sh");
                if (v->flags & TASK_VMA_FLAG_LOCKED)
                    text_string(s, " lo");
                text_string(s, "\n");
            }
        }
    }
    if (!rc && rollup && first != UINT64_MAX) {
        text_hex(s, first, 16);
        text_string(s, "-");
        text_hex(s, last, 16);
        text_string(s, " ---p 00000000 00:00 0 [rollup]\n");
        smaps_fields(&total, s);
    }
    kernel_free(counts.entries);
    return rc;
}
/** @brief Stream global memory data using actual allocator/cache/tmpfs gauges.
 * @param s Required output. No fabricated dirty/writeback/slab estimates.
 */
static void meminfo(struct text_stream *s)
{
    uint64_t cache, idle;
    page_cache_memory_kib(&cache, &idle);
    uint64_t free = mm_free_memory_kib(), total = mm_total_memory_kib();
    field(s, "MemTotal: ", total, " kB\n");
    field(s, "MemFree: ", free, " kB\n");
    field(s, "MemAvailable: ", free, " kB\n");
    field(s, "Cached: ", cache, " kB\n");
    field(s, "Shmem: ", tmpfs_memory_kib(), " kB\n");
    text_string(s, "SwapTotal: 0 kB\nSwapFree: 0 kB\n");
}
/** @brief Render supported extended proc files from their real data owners.
 * @param path Intrinsic key. @param s Validated output slice.
 * @return Zero, ENOENT or real authorization/backend failure; execution-owned.
 */
int proc_extra_emit(const char *path, struct text_stream *s)
{
    if (!__builtin_strncmp(path, "/proc/net/", 10))
        return net_proc_emit(s, path + 10);
    if (!__builtin_strcmp(path, "/proc/loadavg")) {
        uint64_t loads[3];
        sched_load_averages(loads);
        for (unsigned i = 0; i < 3; ++i) {
            uint64_t rounded = loads[i] + 327;
            if (i)
                text_string(s, " ");
            text_unsigned(s, rounded >> 16);
            text_string(s, ".");
            uint64_t fraction = ((rounded & 65535) * 100) >> 16;
            if (fraction < 10)
                text_string(s, "0");
            text_unsigned(s, fraction);
        }
        struct sched_proc_stats st;
        sched_proc_snapshot(&st);
        text_string(s, " ");
        text_unsigned(s, st.running);
        text_string(s, "/");
        text_unsigned(s, st.tasks);
        field(s, " ", st.last_pid, "\n");
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/meminfo")) {
        meminfo(s);
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/partitions"))
        return block_proc_emit(s, false);
    if (!__builtin_strcmp(path, "/proc/diskstats"))
        return block_proc_emit(s, true);
    if (!__builtin_strcmp(path, "/proc/interrupts")) {
        irq_proc_emit(s);
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/swaps")) {
        text_string(s, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/sys/kernel/pid_max")) {
        field(s, "", UINT32_MAX, "\n");
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/vmstat")) {
        uint64_t cache, idle;
        page_cache_memory_kib(&cache, &idle);
        struct mm_proc_stats m;
        mm_proc_snapshot(&m);
        field(s, "nr_free_pages ", mm_free_memory_kib() / 4, "\n");
        field(s, "nr_file_pages ", cache / 4, "\n");
        field(s, "nr_shmem ", tmpfs_memory_kib() / 4, "\n");
        field(s, "pgalloc_normal ", m.allocated, "\n");
        field(s, "pgfree ", m.freed, "\n");
        field(s, "pgfault ", m.faults, "\n");
        text_string(s, "pswpin 0\npswpout 0\n");
        return 0;
    }
    if (!__builtin_strcmp(path, "/proc/stat")) {
        uint64_t user[SMP_MAX_CPUS] = {0}, system[SMP_MAX_CPUS] = {0}, idle[SMP_MAX_CPUS] = {0};
        uint64_t u = 0, k = 0, d = 0;
        uint32_t n = smp_cpu_count();
        if (n > SMP_MAX_CPUS)
            n = SMP_MAX_CPUS;
        sched_cpu_times(user, system, idle, n);
        for (unsigned c = 0; c < n; ++c) {
            u += user[c];
            k += system[c];
            d += idle[c];
        }
        for (unsigned c = 0; c <= n; ++c) {
            text_string(s, "cpu");
            if (c) {
                text_unsigned(s, c - 1);
                u = user[c - 1];
                k = system[c - 1];
                d = idle[c - 1];
            }
            field(s, " ", u, " 0 ");
            field(s, "", k, " ");
            text_unsigned(s, d);
            text_string(s, " 0 0 0 0 0 0\n");
        }
        struct sched_proc_stats st;
        sched_proc_snapshot(&st);
        field(s, "intr ", irq_proc_total(), "\n");
        field(s, "ctxt ", st.switches, "\n");
        struct linux_timespec now;
        if (!time_clock_get(LINUX_CLOCK_REALTIME, &now))
            field(s, "btime ", now.tv_sec - (int64_t)(time_uptime_ms() / 1000), "\n");
        field(s, "processes ", st.created, "\n");
        field(s, "procs_running ", st.running, "\n");
        field(s, "procs_blocked ", st.blocked, "\n");
        return 0;
    }
    if (!__builtin_strncmp(path, "/proc/sys/fs/", 13)) {
        const char *name = path + 13;
        if (!__builtin_strcmp(name, "nr_open"))
            field(s, "", SCHED_NR_OPEN, "\n");
        else if (!__builtin_strcmp(name, "file-max"))
            field(s, "", UINT64_MAX, "\n");
        else if (!__builtin_strcmp(name, "file-nr")) {
            text_unsigned(s, proc_open_file_count());
            text_string(s, "\t0\t");
            field(s, "", UINT64_MAX, "\n");
        } else if (!__builtin_strcmp(name, "pipe-max-size"))
            field(s, "", proc_pipe_policy(0), "\n");
        else if (!__builtin_strcmp(name, "pipe-user-pages-soft"))
            field(s, "", proc_pipe_policy(1), "\n");
        else if (!__builtin_strcmp(name, "pipe-user-pages-hard"))
            field(s, "", proc_pipe_policy(2), "\n");
        else
            return -ENOENT;
        return 0;
    }
    const char *leaf = NULL;
    struct task *t = proc_view_task(path, &leaf);
    if (!t || !leaf)
        return -ENOENT;
    bool sensitive = !__builtin_strcmp(leaf, "maps") || !__builtin_strcmp(leaf, "smaps") ||
                     !__builtin_strcmp(leaf, "smaps_rollup") || !__builtin_strcmp(leaf, "auxv") ||
                     !__builtin_strcmp(leaf, "io") || fdinfo_number(leaf) >= 0;
    if (sensitive && !proc_stat_mm_visible(sched_current_task(), t))
        return -EACCES;
    if (!__builtin_strcmp(leaf, "status")) {
        status(t, s);
        return 0;
    }
    if (!__builtin_strcmp(leaf, "limits")) {
        limits(t, s);
        return 0;
    }
    if (!__builtin_strcmp(leaf, "maps") || !__builtin_strcmp(leaf, "smaps") ||
        !__builtin_strcmp(leaf, "smaps_rollup"))
        return maps(t, leaf, s);
    if (!__builtin_strcmp(leaf, "statm")) {
        struct memory_totals m = {0};
        virtual_totals(t, &m);
        struct map_counts counts = {0};
        int rc = build_map_counts(&counts);
        if (rc)
            return rc;
        uint64_t at = RELIEFNT_USER_BASE, page, phys, flags;
        while (address_space_user_page_next(sched_task_as(t), at, RELIEFNT_USER_TOP, &page, &phys,
                                            &flags)) {
            at = page + 4096;
            struct page_count *entry = page_slot(&counts, phys);
            if (!(flags & RELIEFNT_PAGE_DEVICE) &&
                (page_cache_owns(phys) || (entry && entry->count > 1)))
                m.shared += 4096;
        }
        kernel_free(counts.entries);
        text_unsigned(s, m.size / 4096);
        field(s, " ", m.rss / 4096, " ");
        text_unsigned(s, m.shared / 4096);
        field(s, " ", m.text / 4096, " 0 ");
        text_unsigned(s, (m.data + m.stack) / 4096);
        text_string(s, " 0\n");
        return 0;
    }
    if (!__builtin_strcmp(leaf, "auxv")) {
        struct task_address_space_state *mm = sched_task_mm(t);
        text_bytes(s, mm->saved_auxv, mm->saved_auxv_bytes);
        return 0;
    }
    if (!__builtin_strcmp(leaf, "io")) {
        uint64_t values[6] = {0};
        bool group = t->pid == sched_task_tgid(t);
        for (const char *p = path; *p; ++p)
            if (!__builtin_strncmp(p, "/task/", 6) || !__builtin_strncmp(p, "/thread-self", 12))
                group = false;
        struct task *other;
        for (uint32_t i = 0; (other = sched_task_at(i)); ++i) {
            if (group ? sched_task_tgid(other) != sched_task_tgid(t) : other != t)
                continue;
            values[0] += other->proc_read_chars;
            values[1] += other->proc_write_chars;
            values[2] += other->proc_read_calls;
            values[3] += other->proc_write_calls;
            values[4] += other->proc_read_bytes;
            values[5] += other->proc_write_bytes;
        }
        const char *labels[] = {
            "rchar: ", "wchar: ", "syscr: ", "syscw: ", "read_bytes: ", "write_bytes: "};
        for (unsigned i = 0; i < 6; ++i)
            field(s, labels[i], values[i], "\n");
        text_string(s, "cancelled_write_bytes: 0\n");
        return 0;
    }
    int fd = fdinfo_number(leaf);
    if (fd >= 0) {
        struct task_pty_fd *pty;
        struct task_file *f = view_file(t, fd, &pty);
        if (!f && !pty)
            return -ENOENT;
        uint32_t flags =
            f ? f->flags : (pty->description ? pty->description->status_flags : pty->status_flags);
        uint32_t fdflags = 0;
        struct task_file *descriptor = task_descriptor_for_fd(t, fd);
        if (descriptor)
            fdflags = descriptor->fd_flags;
        else if (pty)
            fdflags = pty->flags;
        uint32_t public_flags =
            flags & (LINUX_O_ACCMODE | LINUX_O_APPEND | LINUX_O_NONBLOCK | LINUX_O_SYNC |
                     LINUX_O_DIRECT | LINUX_O_NOATIME | LINUX_O_DSYNC);
        if (flags & TASK_FILE_FLAG_PATH)
            public_flags |= LINUX_O_PATH;
        if (fdflags)
            public_flags |= LINUX_O_CLOEXEC;
        field(s, "pos:\t", f ? f->offset : 0, "\n");
        text_string(s, "flags:\t");
        octal(s, public_flags);
        text_string(s, "\n");
        const struct vfs_path *p = f                         ? &f->path_ref
                                   : pty && pty->description ? &pty->description->path_ref
                                                             : NULL;
        field(s, "mnt_id:\t", vfs_path_mount_id(p), "\n");
        if (f && f->socket_ino)
            field(s, "ino:\t", f->socket_ino, "\n");
        else if (p && p->mount) {
            struct linux_stat_abi st;
            int rc = vfs_path_stat(p, &st);
            if (rc)
                return rc;
            field(s, "ino:\t", st.st_ino, "\n");
        }
        return f ? proc_fd_details(f, s) : 0;
    }
    return -ENOENT;
}
/** @brief Enumerate a fixed list with deterministic rewind/EOF handling.
 * @param names Table. @param count Entries. @param offset In/out. @param entry Output.
 * @return One or EOF; no allocation or resource lifetime changes.
 */
static int entries(const char *const *names, size_t count, uint64_t *offset,
                   struct reliefos_dir_entry *entry)
{
    if (*offset >= count)
        return 0;
    *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_FILE};
    const char *name = names[(*offset)++];
    for (size_t i = 0; (entry->name[i] = name[i]); ++i) {
    }
    return 1;
}
/** @brief Enumerate net/sysctl/task/fdinfo without truncating descriptor/task tables.
 * @param path Key. @param offset Required cookie. @param entry Required output.
 * @return Entry/EOF/errno; ENOTDIR delegates unrelated directories to legacy code.
 */
int proc_extra_readdir(const char *path, uint64_t *offset, struct reliefos_dir_entry *entry)
{
    if (!__builtin_strcmp(path, "/proc/net"))
        return entries(net_leaves, sizeof(net_leaves) / sizeof(*net_leaves), offset, entry);
    if (!__builtin_strcmp(path, "/proc/sys/fs"))
        return entries(fs_leaves, sizeof(fs_leaves) / sizeof(*fs_leaves), offset, entry);
    const char *leaf = NULL;
    struct task *t = proc_view_task(path, &leaf);
    if (!t || !leaf)
        return -ENOTDIR;
    if (!__builtin_strcmp(leaf, "task")) {
        struct task *selected = NULL, *p;
        for (uint32_t i = 0; (p = sched_task_at(i)); ++i)
            if (p->pid && p->pid >= *offset && sched_task_tgid(p) == sched_task_tgid(t) &&
                (!selected || p->pid < selected->pid))
                selected = p;
        if (!selected)
            return 0;
        *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_DIR};
        struct text_stream s = {.buffer = entry->name, .capacity = sizeof(entry->name) - 1};
        text_unsigned(&s, selected->pid);
        *offset = (uint64_t)selected->pid + 1;
        return 1;
    }
    if (!__builtin_strcmp(leaf, "fdinfo")) {
        if (!proc_stat_mm_visible(sched_current_task(), t))
            return -EACCES;
        uint64_t limit = (uint64_t)sched_task_file_capacity(t) + 3;
        for (uint32_t i = 0; i < sched_task_pty_capacity(t); ++i) {
            struct task_pty_fd *p = sched_task_pty_at(t, i);
            if (p && p->used && p->fd >= 0 && (uint64_t)p->fd + 1 > limit)
                limit = (uint64_t)p->fd + 1;
        }
        while (*offset < limit) {
            uint32_t fd = (*offset)++;
            struct task_pty_fd *pty;
            if (!view_file(t, fd, &pty) && !pty)
                continue;
            *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_FILE};
            struct text_stream s = {.buffer = entry->name, .capacity = sizeof(entry->name) - 1};
            text_unsigned(&s, fd);
            return 1;
        }
        return 0;
    }
    return -ENOTDIR;
}

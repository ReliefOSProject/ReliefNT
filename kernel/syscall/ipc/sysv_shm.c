/* Native LP64 SysV SHM. Registry pages, PTE refs, and VMA identities are
 * separate ownerships. The superseded draft remains in task-19-before-*.
 */
#include <reliefnt/sysv_shm.h>
#include <reliefnt/syscall.h>
#include <reliefnt/sched.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>
#include <linux/shm.h>
#include <linux/capability.h>
#include <linux/mman.h>
#include <limits.h>

#define SYSV_SHM_SLOTS 4096u
#define SYSV_SHM_ID_BITS 12u
#define SYSV_SHM_PAGE 4096ULL
#define SYSV_SHM_MAX_PAGES 65536u
#define SYSV_SHM_MAX_BYTES (SYSV_SHM_MAX_PAGES * SYSV_SHM_PAGE)
#define SYSV_SHM_DEST 01000u
#define SYSV_SHM_LOCKED 02000u

struct sysv_shm_segment {
    struct shmid64_ds stat;
    uint64_t *pages;
    uint32_t page_count, slot, locked_uid, pending_attach;
    int32_t id;
    bool deleted;
};
struct sysv_shm_attachment {
    struct sysv_shm_segment *segment;
    uint64_t base, identity, fork_origin;
    uint32_t pieces;
};
static struct sysv_shm_segment *sysv_shm_ids[SYSV_SHM_SLOTS];
static uint32_t sysv_shm_generations[SYSV_SHM_SLOTS];
static uint32_t sysv_shm_used, sysv_shm_cursor;
static uint64_t sysv_shm_pages, sysv_shm_identity;

/** @brief Read the realtime clock for IPC metadata. @return Seconds. */
static long sysv_shm_now(void)
{
    struct linux_timespec now = {0};
    (void)time_clock_get(LINUX_CLOCK_REALTIME, &now);
    return (long)now.tv_sec;
}
/** @brief Check effective and supplementary group membership.
 * @param task Caller credentials. @param gid Segment owner or creator group.
 * @return True when the caller belongs to that group.
 */
static bool sysv_shm_group(const struct task *task, uint32_t gid)
{
    if (task->egid == gid) return true;
    if (task->groups) for (uint32_t i = 0; i < task->groups->count; ++i)
        if (task->groups->ids[i] == gid) return true;
    return false;
}
/** @brief Apply owner/group/other DAC and CAP_IPC_OWNER.
 * @param task Caller credentials. @param segment Live registry entry.
 * @param requested Linux permission bits, collapsed into read/write/execute.
 * @return True when DAC or the effective capability permits access.
 */
static bool sysv_shm_access(const struct task *task,
                            const struct sysv_shm_segment *segment, uint32_t requested)
{
    const struct ipc64_perm *perm = &segment->stat.shm_perm;
    uint32_t mode = perm->mode;
    requested = (requested | (requested >> 3) | (requested >> 6)) & 7;
    if (task->euid == perm->uid || task->euid == perm->cuid) mode >>= 6;
    else if (sysv_shm_group(task, perm->gid) || sysv_shm_group(task, perm->cgid)) mode >>= 3;
    return !(requested & ~mode) || (task->cap_effective & (1ULL << CAP_IPC_OWNER));
}
/** @brief Check IPC_SET/RMID ownership.
 * @param task Caller credentials. @param segment Live registry entry.
 * @return True for its owner, creator, or CAP_SYS_ADMIN.
 */
static bool sysv_shm_owner(const struct task *task, const struct sysv_shm_segment *segment)
{
    return task->euid == segment->stat.shm_perm.uid || task->euid == segment->stat.shm_perm.cuid ||
        (task->cap_effective & (1ULL << CAP_SYS_ADMIN));
}
/** @brief Resolve a generation ID or SHM_STAT slot.
 * @param id Nonnegative native ID or slot. @param index Select a slot lookup.
 * @return Borrowed live entry, including one marked RMID, or NULL.
 */
static struct sysv_shm_segment *sysv_shm_find(int32_t id, bool index)
{
    if (id < 0 || (index && (uint32_t)id >= SYSV_SHM_SLOTS)) return NULL;
    struct sysv_shm_segment *s = sysv_shm_ids[(uint32_t)id & (SYSV_SHM_SLOTS - 1)];
    return s && (index || s->id == id) ? s : NULL;
}
/** @brief Release registry-owned page references and the retired segment.
 * @param segment Unattached entry with no pending attach operation.
 * @return No value; page table references retain their own backing ownership.
 */
static void sysv_shm_destroy(struct sysv_shm_segment *segment)
{
    sysv_shm_ids[segment->slot] = NULL;
    --sysv_shm_used; sysv_shm_pages -= segment->page_count;
    for (uint32_t i = 0; i < segment->page_count; ++i) mm_free_page(segment->pages[i]);
    kernel_free(segment->pages); kernel_free(segment);
}
/** @brief Return the highest occupied slot for IPC_INFO/SHM_INFO.
 * @return Highest live slot, or zero for an empty registry.
 */
static int sysv_shm_max_index(void)
{
    for (uint32_t i = SYSV_SHM_SLOTS; i; --i) if (sysv_shm_ids[i - 1]) return (int)i - 1;
    return 0;
}
/** @brief Create or find a key and publish only after all pages are zeroed.
 * @param task Caller and segment creator. @param key Native IPC key.
 * @param size Logical byte count. @param flags IPC creation and permission bits.
 * @return Generation-bearing ID or negative errno; failures publish no entry.
 */
static int64_t sysv_shm_get(struct task *task, int32_t key, uint64_t size, uint32_t flags)
{
    if (key != IPC_PRIVATE) {
        for (uint32_t i = 0; i < SYSV_SHM_SLOTS; ++i) {
            struct sysv_shm_segment *s = sysv_shm_ids[i];
            if (!s || s->deleted || s->stat.shm_perm.key != key) continue;
            if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL)) return -LINUX_EEXIST;
            if (size > s->stat.shm_segsz) return -LINUX_EINVAL;
            return sysv_shm_access(task, s, flags & 0777) ? s->id : -LINUX_EACCES;
        }
        if (!(flags & IPC_CREAT)) return -LINUX_ENOENT;
    }
    if (!size || size > SYSV_SHM_MAX_BYTES) return -LINUX_EINVAL;
    if (flags & SHM_HUGETLB) return -LINUX_ENOSYS;
    uint32_t count = (uint32_t)((size + SYSV_SHM_PAGE - 1) / SYSV_SHM_PAGE);
    if (sysv_shm_used == SYSV_SHM_SLOTS || count > SYSV_SHM_MAX_PAGES - sysv_shm_pages)
        return -LINUX_ENOSPC;
    struct sysv_shm_segment *s = kernel_malloc(sizeof(*s));
    if (!s) return -LINUX_ENOMEM;
    *s = (struct sysv_shm_segment){0};
    s->pages = kernel_malloc((uint64_t)count * sizeof(*s->pages));
    if (!s->pages) { kernel_free(s); return -LINUX_ENOMEM; }
    uint32_t made = 0;
    for (; made < count; ++made) {
        s->pages[made] = mm_alloc_page();
        if (!s->pages[made]) break;
        __builtin_memset((void *)(uintptr_t)s->pages[made], 0, SYSV_SHM_PAGE);
    }
    if (made != count) {
        while (made) mm_free_page(s->pages[--made]);
        kernel_free(s->pages); kernel_free(s); return -LINUX_ENOMEM;
    }
    uint32_t slot = sysv_shm_cursor;
    while (sysv_shm_ids[slot]) slot = (slot + 1) % SYSV_SHM_SLOTS;
    sysv_shm_cursor = (slot + 1) % SYSV_SHM_SLOTS;
    uint32_t generation = sysv_shm_generations[slot]++;
    if (generation >= ((uint32_t)INT32_MAX >> SYSV_SHM_ID_BITS)) {
        generation = 0; sysv_shm_generations[slot] = 1;
    }
    s->slot = slot; s->id = (int32_t)((generation << SYSV_SHM_ID_BITS) | slot);
    s->page_count = count;
    s->stat.shm_perm = (struct ipc64_perm){.key = key, .uid = task->euid, .gid = task->egid,
        .cuid = task->euid, .cgid = task->egid, .mode = flags & 0777,
        .seq = (unsigned short)generation};
    s->stat.shm_segsz = size; s->stat.shm_ctime = sysv_shm_now();
    s->stat.shm_cpid = (int)sched_task_tgid(task);
    sysv_shm_ids[slot] = s; ++sysv_shm_used; sysv_shm_pages += count;
    return s->id;
}
/** @brief Attach a segment, including an RMID entry held by existing VMAs.
 * @param task Current user address space. @param id Native segment ID.
 * @param address Requested address or zero. @param flags Native SHM attach flags.
 * @return User address or negative errno, with no new attachment on failure.
 */
static int64_t sysv_shm_attach(struct task *task, int32_t id, uint64_t address, uint32_t flags)
{
    if (id < 0) return -LINUX_EINVAL;
    if (address & (SYSV_SHM_PAGE - 1)) {
        if (flags & SHM_RND) address &= ~(SYSV_SHM_PAGE - 1);
        else return -LINUX_EINVAL;
    }
    if (!address && (flags & SHM_REMAP)) return -LINUX_EINVAL;
    struct sysv_shm_segment *s = sysv_shm_find(id, false);
    if (!s) return -LINUX_EINVAL;
    uint32_t requested = flags & SHM_RDONLY ? 4 : 6;
    if (flags & SHM_EXEC) requested |= 1;
    if (!sysv_shm_access(task, s, requested)) return -LINUX_EACCES;
    uint32_t prot = LINUX_PROT_READ | (flags & SHM_RDONLY ? 0 : LINUX_PROT_WRITE);
    if (flags & SHM_EXEC) prot |= LINUX_PROT_EXEC;
    if ((prot & (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) ==
        (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) return -LINUX_EACCES;
    struct sysv_shm_attachment *a = kernel_malloc(sizeof(*a));
    if (!a) return -LINUX_ENOMEM;
    *a = (struct sysv_shm_attachment){.segment = s, .identity = ++sysv_shm_identity, .pieces = 1};
    ++s->pending_attach;
    int64_t mapped = syscall_mm_map_sysv_shm(task, address,
        (uint64_t)s->page_count * SYSV_SHM_PAGE, prot, prot | LINUX_PROT_EXEC,
        (flags & SHM_REMAP) != 0, a, s->pages);
    --s->pending_attach;
    if (mapped < 0) {
        kernel_free(a);
        if (s->deleted && !s->stat.shm_nattch && !s->pending_attach) sysv_shm_destroy(s);
        return mapped;
    }
    a->base = (uint64_t)mapped; ++s->stat.shm_nattch;
    s->stat.shm_atime = sysv_shm_now(); s->stat.shm_lpid = (int)sched_task_tgid(task);
    return mapped;
}
/** @brief Retain a split VMA piece and reflect native nattch behavior.
 * @param vma Newly copied piece whose attachment identity is already assigned.
 * @return No value; non-SHM VMAs are ignored.
 */
void sysv_shm_vma_split(struct task_vma *vma)
{
    if (!vma || !vma->sysv_shm_attachment) return;
    ++vma->sysv_shm_attachment->pieces;
    ++vma->sysv_shm_attachment->segment->stat.shm_nattch;
}
/** @brief Clone an attachment into a new MM, grouping its split pieces.
 * @param child New address space whose copied metadata is being initialized.
 * @param dst Destination VMA, without an owned attachment reference yet.
 * @param src Parent VMA with a live attachment.
 * @return Zero or -ENOMEM; failed destination owns no SHM reference.
 */
int sysv_shm_vma_clone(struct task *child, struct task_vma *dst, const struct task_vma *src)
{
    dst->sysv_shm_attachment = NULL;
    struct sysv_shm_attachment *source = src->sysv_shm_attachment;
    if (!source) return 0;
    for (uint32_t i = 0; i < sched_task_vma_capacity(child); ++i) {
        struct task_vma *v = sched_task_vma_at(child, i);
        if (!v || v == dst || !v->sysv_shm_attachment ||
            v->sysv_shm_attachment->fork_origin != source->identity) continue;
        dst->sysv_shm_attachment = v->sysv_shm_attachment; sysv_shm_vma_split(dst); return 0;
    }
    struct sysv_shm_attachment *a = kernel_malloc(sizeof(*a));
    if (!a) return -LINUX_ENOMEM;
    *a = (struct sysv_shm_attachment){.segment = source->segment, .base = source->base,
        .identity = ++sysv_shm_identity, .fork_origin = source->identity, .pieces = 1};
    dst->sysv_shm_attachment = a; ++a->segment->stat.shm_nattch;
    a->segment->stat.shm_atime = sysv_shm_now(); a->segment->stat.shm_lpid = (int)sched_task_tgid(child);
    return 0;
}
/** @brief Release one VMA piece and destroy an RMID segment after last detach.
 * @param task Detaching process used for lpid, or NULL when unavailable.
 * @param vma Owned VMA piece whose attachment pointer is cleared.
 * @return No value; registry references are retired only after the last piece.
 */
void sysv_shm_vma_release(struct task *task, struct task_vma *vma)
{
    if (!vma || !vma->sysv_shm_attachment) return;
    struct sysv_shm_attachment *a = vma->sysv_shm_attachment;
    struct sysv_shm_segment *s = a->segment; vma->sysv_shm_attachment = NULL;
    if (s->stat.shm_nattch) --s->stat.shm_nattch;
    s->stat.shm_dtime = sysv_shm_now(); if (task) s->stat.shm_lpid = (int)sched_task_tgid(task);
    if (a->pieces && !--a->pieces) kernel_free(a);
    if (s->deleted && !s->stat.shm_nattch && !s->pending_attach) sysv_shm_destroy(s);
}
/** @brief Detach all remaining split pieces for one original shmat address.
 * @param task Current user address space. @param address Original shmat base.
 * @return Zero or negative errno; unrelated and adjacent VMAs stay live.
 */
static int64_t sysv_shm_detach(struct task *task, uint64_t address)
{
    if (!address || (address & (SYSV_SHM_PAGE - 1))) return -LINUX_EINVAL;
    struct sysv_shm_attachment *a = NULL;
    uint64_t first = UINT64_MAX;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *v = sched_task_vma_at(task, i);
        /* VMA slots are unordered. A remapped prefix may share its original
         * base with an older attachment's tail: Linux selects the first VMA
         * in address order, not the first metadata slot. */
        if (v && v->used && v->sysv_shm_attachment &&
            v->sysv_shm_attachment->base == address && v->start < first) {
            a = v->sysv_shm_attachment;
            first = v->start;
        }
    }
    if (!a) return -LINUX_EINVAL;
    for (;;) {
        struct task_vma *piece = NULL;
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *v = sched_task_vma_at(task, i);
            if (v && v->used && v->sysv_shm_attachment == a) { piece = v; break; }
        }
        if (!piece) return 0;
        bool last = a->pieces == 1;
        int ret = (int)syscall_mm_munmap(piece->start, piece->end - piece->start);
        if (ret) return ret;
        if (last) return 0;
    }
}
/** @brief Lock or unlock resident pages under owner and memlock policy.
 * @param task Caller credentials and shared resource limits.
 * @param s Live segment. @param lock True for SHM_LOCK, false for SHM_UNLOCK.
 * @return Zero or negative errno, without changing a denied segment's mode.
 */
static int sysv_shm_lock(struct task *task, struct sysv_shm_segment *s, bool lock)
{
    bool privileged = task->cap_effective & (1ULL << CAP_IPC_LOCK);
    if (!privileged && task->euid != s->stat.shm_perm.uid &&
        task->euid != s->stat.shm_perm.cuid) return -LINUX_EPERM;
    uint64_t limit = sched_task_limits(task)->memlock.rlim_cur;
    if (!privileged && lock && !limit) return -LINUX_EPERM;
    if (lock && !(s->stat.shm_perm.mode & SYSV_SHM_LOCKED)) {
        uint64_t held = 0, bytes = (uint64_t)s->page_count * SYSV_SHM_PAGE;
        for (uint32_t i = 0; i < SYSV_SHM_SLOTS; ++i) {
            struct sysv_shm_segment *o = sysv_shm_ids[i];
            if (o && (o->stat.shm_perm.mode & SYSV_SHM_LOCKED) &&
                o->locked_uid == task->euid)
                held += (uint64_t)o->page_count * SYSV_SHM_PAGE;
        }
        if (!privileged && (bytes > limit || held > limit - bytes)) return -LINUX_ENOMEM;
        s->locked_uid = task->euid; s->stat.shm_perm.mode |= SYSV_SHM_LOCKED;
    } else if (!lock) s->stat.shm_perm.mode &= ~SYSV_SHM_LOCKED;
    return 0;
}
/** @brief Dispatch canonical LP64 shmctl structures and commands.
 * @param task Caller credentials and output address space.
 * @param id Native ID or SHM_STAT slot. @param command Native unmodified command.
 * @param pointer User structure address, ignored for RMID and LOCK/UNLOCK.
 * @return Native command result or negative errno.
 */
static int64_t sysv_shm_control(struct task *task, int32_t id, int32_t command, uint64_t pointer)
{
    if (id < 0 || command < 0) return -LINUX_EINVAL;
    if (command == IPC_INFO) {
        struct shminfo64 info = {.shmmax = SYSV_SHM_MAX_BYTES, .shmmin = 1,
            .shmmni = SYSV_SHM_SLOTS, .shmseg = SYSV_SHM_SLOTS,
            .shmall = SYSV_SHM_MAX_PAGES};
        return user_copy_to_task(task, pointer, &info, sizeof(info)) ?
            -LINUX_EFAULT : sysv_shm_max_index();
    }
    if (command == SHM_INFO) {
        struct shm_info info = {.used_ids = (int)sysv_shm_used,
            .shm_tot = sysv_shm_pages, .shm_rss = sysv_shm_pages};
        return user_copy_to_task(task, pointer, &info, sizeof(info)) ?
            -LINUX_EFAULT : sysv_shm_max_index();
    }
    bool stat = command == IPC_STAT || command == SHM_STAT || command == SHM_STAT_ANY;
    struct shmid64_ds input;
    if (!stat && command != IPC_SET && command != IPC_RMID &&
        command != SHM_LOCK && command != SHM_UNLOCK) return -LINUX_EINVAL;
    if (command == IPC_SET) {
        if (!user_range_ok(pointer, sizeof(input))) return -LINUX_EFAULT;
        __builtin_memcpy(&input, (const void *)(uintptr_t)pointer, sizeof(input));
    }
    struct sysv_shm_segment *s = sysv_shm_find(id, command == SHM_STAT || command == SHM_STAT_ANY);
    if (!s) return -LINUX_EINVAL;
    if (command == IPC_RMID || command == IPC_SET) {
        if (!sysv_shm_owner(task, s)) return -LINUX_EPERM;
        if (command == IPC_RMID) {
            s->deleted = true;
            s->stat.shm_perm.key = IPC_PRIVATE;
            s->stat.shm_perm.mode |= SYSV_SHM_DEST;
            if (!s->stat.shm_nattch && !s->pending_attach) sysv_shm_destroy(s);
        } else {
            if (input.shm_perm.uid == UINT32_MAX || input.shm_perm.gid == UINT32_MAX)
                return -LINUX_EINVAL;
            s->stat.shm_perm.uid = input.shm_perm.uid;
            s->stat.shm_perm.gid = input.shm_perm.gid;
            s->stat.shm_perm.mode = (s->stat.shm_perm.mode & ~0777u) |
                (input.shm_perm.mode & 0777);
            s->stat.shm_ctime = sysv_shm_now();
        }
        return 0;
    }
    if (command == SHM_LOCK || command == SHM_UNLOCK)
        return sysv_shm_lock(task, s, command == SHM_LOCK);
    if (command != SHM_STAT_ANY && !sysv_shm_access(task, s, 4)) return -LINUX_EACCES;
    return user_copy_to_task(task, pointer, &s->stat, sizeof(s->stat)) ?
        -LINUX_EFAULT : (command == IPC_STAT ? 0 : s->id);
}
/** @brief Execute native shmget/shmat/shmdt/shmctl; no private SHM fallback.
 * @param number Canonical x86_64 syscall number.
 * @param a0 First native argument. @param a1 Second native argument.
 * @param a2 Third native argument.
 * @return Native result or negative errno; unknown syscall numbers return ENOSYS.
 */
int64_t syscall_sysv_shm(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2)
{
    struct task *task = sched_current_task(); if (!task) return -LINUX_ESRCH;
    switch (number) {
    case __NR_shmget: return sysv_shm_get(task, (int32_t)a0, a1, (uint32_t)a2);
    case __NR_shmat: return sysv_shm_attach(task, (int32_t)a0, a1, (uint32_t)a2);
    case __NR_shmdt: return sysv_shm_detach(task, a0);
    case __NR_shmctl: return sysv_shm_control(task, (int32_t)a0, (int32_t)a1, a2);
    default: return -LINUX_ENOSYS;
    }
}

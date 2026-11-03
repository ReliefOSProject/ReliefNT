/* Reproduce musl pthread_create's PROT_NONE stack followed by mprotect.
 * Link the real growable scheduler VMA store and exercise actual paging under
 * ASan so both retained pointers and lazy-page access are checked. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/sched.h>
#include "../../kernel/sched/internal.h"
#define RELIEFNT_TEST_REAL_EXEC_SOURCE
#include "exec_file_fixture.h"

static struct task *current;
#ifdef TASK8_FIX2_DEVICE_MMAP
static uint64_t fix2_device_user_start,fix2_device_user_end;
#endif
static struct task *test_current_task(void) { return current; }
#define sched_current_task test_current_task
#include "paging_cpu_fixture.h"
#include "../../arch/x86_64/mm/paging.c"
#define RELIEFNT_MM_TEST_STATS
#include "../../mm/user/context.c"
static unsigned lock_population_calls;
static unsigned execution_suspends, execution_resumes;
static unsigned last_suspend_calls, largest_suspend_batch;
static unsigned long long lock_slot_visits, last_suspend_slots, largest_slot_batch;
static bool track_lock_slots;
static struct task *progress_task;
static uint64_t progress_base;
static bool progress_seen[100];
static unsigned progress_unique, progress_repeats;
static void (*execution_reentry)(void);
static bool execution_suspend_fails;
static int counted_lock_population(struct task *task, uint64_t page);
static int counted_lock_vma_population(struct task *task, struct task_vma *vma, uint64_t page);
static int counted_lock_stack_population(struct task *task, uint64_t page);
static struct task_vma *counted_lock_slot(struct task *task, uint32_t index);
#define sched_task_vma_at counted_lock_slot
#define task_populate_lock_page counted_lock_population
#ifdef TASK8_FIX3_REUSED_BACKING
static bool fail_fix3_vma_publication;
static int fix3_task_vma_record_mapping(struct task *task,uint64_t start,uint64_t end,
    uint64_t prot,uint32_t flags,const struct storage_node *node,uint64_t offset,
    uint32_t max_prot,struct task_file *file);
#define task_vma_record_mapping fix3_task_vma_record_mapping
#endif
#include "../../mm/user/mapping.c"
#ifdef TASK8_FIX3_REUSED_BACKING
#undef task_vma_record_mapping
#endif
#include "../../mm/user/vma.c"
#include "../../mm/user/protect.c"
#include "../../mm/user/cache.c"
#undef task_populate_lock_page
#include "../../mm/user/fault.c"
#define task_populate_lock_page counted_lock_population
#define task_populate_lock_vma_page counted_lock_vma_population
#define task_populate_lock_stack_page counted_lock_stack_population
#include "../../mm/user/locked.c"
#undef task_populate_lock_page
#undef task_populate_lock_vma_page
#undef task_populate_lock_stack_page
#undef sched_task_vma_at
#ifdef TASK8_FIX3_REUSED_BACKING
/** @brief Fail only the final VMA publication requested by the fix3 fixture.
 * @param task Original task. @param start First byte. @param end End byte.
 * @param prot Protection. @param flags VMA flags. @param node Optional node.
 * @param offset File offset. @param max_prot Allowed protection. @param file OFD.
 * @return Injected ENOMEM or the real production publication result.
 */
static int fix3_task_vma_record_mapping(struct task *task,uint64_t start,uint64_t end,
    uint64_t prot,uint32_t flags,const struct storage_node *node,uint64_t offset,
    uint32_t max_prot,struct task_file *file)
{
    if(fail_fix3_vma_publication)return -RELIEFOS_ENOMEM;
    return task_vma_record_mapping(task,start,end,prot,flags,node,offset,max_prot,file);
}
#endif
static struct task_vma *counted_lock_slot(struct task *task, uint32_t index)
{
    if (track_lock_slots) ++lock_slot_visits;
    return sched_task_vma_at(task, index);
}
static int counted_lock_population(struct task *task, uint64_t page)
{
    ++lock_population_calls;
    return task_populate_lock_page(task, page);
}
static int counted_lock_vma_population(struct task *task, struct task_vma *vma, uint64_t page)
{
    ++lock_population_calls;
    if (task == progress_task && page >= progress_base && page < progress_base + 100 * 4096) {
        unsigned index = (unsigned)((page - progress_base) / 4096);
        if (progress_seen[index]) ++progress_repeats;
        else { progress_seen[index] = true; ++progress_unique; }
    }
    return task_populate_lock_vma_page(task, vma, page);
}
static int counted_lock_stack_population(struct task *task, uint64_t page)
{
    ++lock_population_calls;
    return task_populate_lock_stack_page(task, page);
}

/** @brief Inject peer activity at the production suspension boundary.
 * @param token Inactive execution token. @return Zero after simulated release.
 */
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token)
{
    assert(token && !token->active);
    if (execution_suspend_fails) return -35;
    token->active = 1;
    ++execution_suspends;
    unsigned batch = lock_population_calls - last_suspend_calls;
    if (batch > largest_suspend_batch) largest_suspend_batch = batch;
    last_suspend_calls = lock_population_calls;
    unsigned long long slots = lock_slot_visits - last_suspend_slots;
    if (slots > largest_slot_batch) largest_slot_batch = slots;
    last_suspend_slots = lock_slot_visits;
    if (execution_reentry) {
        bool tracked = track_lock_slots;
        track_lock_slots = false;
        execution_reentry();
        track_lock_slots = tracked;
    }
    return 0;
}
/** @brief Complete the simulated production resume. @param token Active token.
 * @return Zero with token consumed.
 */
int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token)
{ assert(token && token->active); token->active = 0; ++execution_resumes; return 0; }
/** @brief Unexpected loss of execution ownership is fatal in the fixture.
 * @param message Fatal diagnostic. Does not return.
 */
__attribute__((noreturn)) void panic(const char *message)
{ fprintf(stderr, "%s\n", message); abort(); }
#undef RELIEFNT_MM_TEST_STATS
#undef sched_current_task

static unsigned pages, allocations, deallocations;
static struct { uint64_t phys; unsigned refs; } page_refs[1024];
static bool fail_page;
static int fail_page_after = -1;
static unsigned emergency_page_attempts;
static unsigned page_retains;
static bool allow_shared_write_grant;
static unsigned shared_write_grants;
static unsigned user_tlb_flush_calls;
static uint64_t user_tlb_flush_probe_page;
static uint64_t retired_probe_page;
static bool retired_probe_flushed;
static uint8_t mincore_bytes[1024];
static struct task_file *drm_test_file = (struct task_file *)(uintptr_t)0x2000;
static bool fail_file_get;
static int fail_file_get_after = -1;
static unsigned file_get_calls, file_put_calls;
static int fail_after = -1;
static bool fail_exec_read;
static unsigned exec_read_calls;
void *kernel_malloc(size_t size)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    ++allocations;
    void *memory = malloc(size);
    assert(memory);
    return memory;
}
void kernel_free(void *memory) { if (memory) ++deallocations; free(memory); }
uint64_t mm_alloc_page(void)
{
    if (fail_page || fail_page_after == 0) return 0;
    if (fail_page_after > 0) --fail_page_after;
    void *memory = aligned_alloc(4096, 4096);
    assert(memory);
    memset(memory, 0, 4096);
    ++pages;
    unsigned i = 0;
    while (i < 1024 && page_refs[i].refs) ++i;
    assert(i < 1024);
    page_refs[i].phys = (uintptr_t)memory;
    page_refs[i].refs = 1;
    return (uintptr_t)memory;
}
/** @brief Allocate a recovery page from the fixture's modeled page pool.
 * @return A zeroed, reference-counted page or zero when the pool is exhausted.
 * The host model has no separate reserve; both entry points obey its failure
 * injection and release the returned reference through mm_free_page.
 */
uint64_t mm_alloc_emergency_page(void)
{ ++emergency_page_attempts; return mm_alloc_page(); }
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
/** @brief Resolve fixture-owned page tables through the host physical model.
 * @param physical Tracked physical address. @return Borrowed host pointer.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical)
{ return mm_phys_to_virt(physical); }
uint64_t mm_virt_to_phys(const void *address) { return (uint64_t)(uintptr_t)address; }
void mm_free_page(uint64_t page)
{
    if (page == retired_probe_page) assert(retired_probe_flushed);
    unsigned i = 0;
    while (i < 1024 && (!page_refs[i].refs || page_refs[i].phys != page)) ++i;
    assert(i < 1024);
    if (!--page_refs[i].refs) { assert(pages); --pages; free((void *)(uintptr_t)page); }
}
/** @brief Retain a fixture-owned physical page. @param page Tracked frame. */
void mm_retain_page(uint64_t page)
{
    ++page_retains;
    unsigned i = 0;
    while (i < 1024 && (!page_refs[i].refs || page_refs[i].phys != page)) ++i;
    assert(i < 1024); ++page_refs[i].refs;
}
uint64_t mm_free_memory_kib(void) { return UINT64_MAX; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
int page_cache_owns(uint64_t page) { return allow_shared_write_grant && page; }
/** @brief Reject fixture cache retention. @param page Candidate frame. @return Miss. */
int page_cache_retain(uint64_t page) { (void)page; return -1; }
/** @brief Model a cache miss. @param node Ignored file. @param offset Ignored offset.
 * @param phys Unused output. @return Negative miss.
 */
int page_cache_lookup(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; return -1; }
void page_cache_release(uint64_t page) { (void)page; abort(); }
void page_cache_mark_shared_writable(uint64_t page)
{ assert(allow_shared_write_grant && page); ++shared_write_grants; }
/** @brief Reject unexpected file I/O. @param node Ignored file. @param offset Offset.
 * @param loader Ignored loader. @param context Ignored state. @param phys Unused output.
 * @return Never returns.
 */
int page_cache_load_fault(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{ (void)node; (void)offset; (void)loader; (void)context; (void)phys; abort(); }
/** @brief This anonymous-only fixture does not admit ordinary file cache I/O. */
int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{ (void)node; (void)offset; (void)loader; (void)context; (void)phys; abort(); }
/** @brief Reject unexpected tmpfs I/O. @param node File. @param offset Offset.
 * @param phys Unused output. @return Never returns.
 */
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; abort(); }
/** @brief Reject unexpected tmpfs queries. @param node File. @param offset Offset.
 * @return Never returns.
 */
bool storage_tmpfs_page_resident(const struct storage_node *node, uint64_t offset)
{ (void)node; (void)offset; abort(); }
/** @brief Enter the single-threaded fixture loader. @param flags Saved state output. */
void userland_loader_lock(uint64_t *flags) { *flags = 0; }
/** @brief Leave the fixture loader. @param flags Saved state. */
void userland_loader_unlock(uint64_t flags) { (void)flags; }
/** @brief Permit only the fixture output vector. @param address Candidate pointer.
 * @param length Required bytes. @return True when the vector fits.
 */
bool user_range_writable(uint64_t address, uint64_t length)
{
#ifdef TASK8_FIX2_DEVICE_MMAP
    if(address>=fix2_device_user_start && address<fix2_device_user_end &&
       length<=fix2_device_user_end-address)return true;
#endif
    return address == (uintptr_t)mincore_bytes && length <= sizeof(mincore_bytes);
}
/** @brief Enter the fixture registry lock. @param lock Lock. @param flags Saved state output. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
/** @brief Leave the fixture registry lock. @param lock Lock. @param flags Saved state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
/** @brief Enter the fixture's nonrecursive MMIO lock. @param lock Stable lock. */
void kernel_spin_lock(struct kernel_spinlock *lock)
{ assert(!lock->state); lock->state = 1; }
/** @brief Leave the fixture's MMIO lock. @param lock Previously held lock. */
void kernel_spin_unlock(struct kernel_spinlock *lock)
{ assert(lock->state); lock->state = 0; }
void x86_64_invlpg(uint64_t page) { (void)page; }
void smp_flush_user_tlb(void)
{
    ++user_tlb_flush_calls;
    if (retired_probe_page) retired_probe_flushed = true;
    if (user_tlb_flush_probe_page)
        assert(!address_space_user_page_writable(
            sched_task_as(current), user_tlb_flush_probe_page));
}


void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
static int exec_fixture_read(void *context,const struct storage_node *node,uint64_t offset,
                               void *buffer,uint32_t length,uint32_t *read,uint64_t mount_flags)
{(void)context;(void)node;(void)offset;(void)buffer;(void)length;(void)read;(void)mount_flags;
 if(fail_exec_read){++exec_read_calls;return -RELIEFOS_EIO;}abort();}
struct task_file *task_file_get(struct task_file *description)
{
    assert(!description || description == drm_test_file);
    ++file_get_calls;
    if (fail_file_get || fail_file_get_after == 0) return NULL;
    if (fail_file_get_after > 0) --fail_file_get_after;
    return description;
}
void task_file_put(struct task_file *description)
{ assert(!description || description == drm_test_file); if (description) ++file_put_calls; }
void sysv_shm_vma_release(struct task *task, struct task_vma *vma)
{ (void)task; (void)vma; }
void sysv_shm_vma_split(struct task_vma *vma) { (void)vma; }
/** @brief Reject unexpected SysV faults. @param task MM. @param vma Mapping.
 * @param page Address. @return Never returns.
 */
int sysv_shm_fault_page(struct task *task, struct task_vma *vma, uint64_t page)
{ (void)task; (void)vma; (void)page; abort(); }
/** @brief Reject unexpected SysV residency. @param vma Mapping. @param page Address.
 * @return Never returns.
 */
bool sysv_shm_page_resident(struct task_vma *vma, uint64_t page)
{ (void)vma; (void)page; abort(); }
/* mremap also links its anonymous/file alternatives. Those providers are
 * absent here and must never be entered by the actual device-remap test. */
#ifdef TASK8_FIX2_DEVICE_MMAP
static struct task_file *fix2_device_file;
#endif
struct task_file *task_file_for_io(struct task *task,int fd)
{
#ifdef TASK8_FIX2_DEVICE_MMAP
    if(task==current && fd==3)return fix2_device_file;
#endif
    (void)task;(void)fd;abort();
}
int file_can_read(const struct task_file *file)
{
#ifdef TASK8_FIX2_DEVICE_MMAP
    if(file==fix2_device_file)return (file->flags&LINUX_O_ACCMODE)!=LINUX_O_WRONLY;
#endif
    (void)file;abort();
}
int file_can_write(const struct task_file *file)
{
#ifdef TASK8_FIX2_DEVICE_MMAP
    if(file==fix2_device_file)return (file->flags&LINUX_O_ACCMODE)!=LINUX_O_RDONLY;
#endif
    (void)file;abort();
}
void storage_set_io_async_context(bool enabled){if(fail_exec_read){assert(!enabled);return;}abort();}
/** @brief Preserve the fixture provider's real negative error. @param error Status.
 * @return Original negative error.
 */
int storage_errno(int error){return error;}
const struct framebuffer *framebuffer_get(void)
{
#if defined(TASK8_FIX4_SCALE) || defined(TASK8_FIX4_CALLBACK)
    return NULL; /* The independent low-MMIO aperture is modeled non-RAM. */
#else
    abort();
#endif
}
int task_shm_mmap_check(const struct task_file *file,uint32_t flags,uint32_t prot,uint32_t *maximum)
{(void)file;(void)flags;(void)prot;(void)maximum;abort();}
static const uint64_t base = RELIEFNT_USER_BASE + 0x100000;

/** @brief A large inaccessible FUTURE guard must not fault once per page. */
static void test_population_guard_operation_count(void);
static void clear_setup_vmas(void);

static void setup(unsigned extra)
{
    current = calloc(1, sizeof(*current));
    assert(current);
    current->kind = TASK_KIND_USER;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(current)));
    unsigned count = SCHED_TASK_VMA_MAX + extra;
    for (unsigned i = 0; i < count; ++i) {
        struct task_vma *vma = sched_task_vma_at(current, i);
        assert(vma);
        *vma = (struct task_vma){.used = 1, .start = base + (i + 1) * 0x10000,
            .end = base + (i + 1) * 0x10000 + 4096,
            .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_NONE,
            .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC};
    }
    struct task_vma *target = sched_task_vma_at(current, count - 1);
    target->start = base;
    target->end = base + 5 * 4096;
}

static void teardown(void)
{
    sched_task_vma_release(current);
    address_space_destroy(sched_task_as(current));
    assert(!pages);
    free(current);
    current = NULL;
}

static void test_population_guard_operation_count(void)
{
    setup(1);
    clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    uint64_t address = 0x10000000000ULL, length = 1ULL << 40;
    lock_population_calls = 0;
    assert(syscall_mm_mmap(address, length, LINUX_PROT_NONE,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)address);
    fprintf(stderr, "guard population calls: %u\n", lock_population_calls);
    assert(lock_population_calls <= 1);
    assert(mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(!syscall_mm_munlockall());
    uint64_t next = address + length + 4096;
    assert(syscall_mm_mmap(next, 4096, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)next);
    lock_population_calls = 0;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    assert(lock_population_calls == 1 && mm_locked_bytes(current) == length + 4096);
    assert(address_space_user_page_phys(sched_task_as(current), next));
    assert(!syscall_mm_mprotect(address, 4096, 3));
    assert(syscall_handle_task_page_fault(current, address, 6));
    teardown();
    puts("PASS 1 TiB FUTURE/CURRENT guard: zero/one population calls, later mprotect and refault");
}

/** @brief Remove unused setup VMAs so operation counts cover only each case. */
static void clear_setup_vmas(void)
{
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
}

/** @brief Reset instrumentation before one production population operation. */
static void reset_population_stats(void)
{
    lock_population_calls = execution_suspends = execution_resumes = 0;
    last_suspend_calls = largest_suspend_batch = 0;
    lock_slot_visits = last_suspend_slots = largest_slot_batch = 0;
    track_lock_slots = false;
    execution_reentry = NULL;
    execution_suspend_fails = false;
}

/** @brief Charge slot searches across many separated inaccessible mappings. */
static void test_population_many_guard_search_budget(void)
{
    const unsigned count = 1024;
    unsigned locked = 0;
    setup(count); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    for (unsigned i = 0; i < count; ++i) {
        struct task_vma *vma = sched_task_vma_at(current, i);
        *vma = (struct task_vma){.used = 1, .start = base + (uint64_t)i * 8192,
            .end = base + (uint64_t)i * 8192 + 4096,
            .prot = i % 3 ? 3 : LINUX_PROT_NONE, .max_prot = 3,
            .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE |
                     (i % 3 == 2 ? 0 : TASK_VMA_FLAG_LOCKED) |
                     (i % 3 == 1 ? TASK_VMA_FLAG_LOCKONFAULT : 0)};
        if (vma->flags & TASK_VMA_FLAG_LOCKED) ++locked;
    }
    reset_population_stats(); track_lock_slots = true;
    assert(!mm_populate_locked_range(base, base + (uint64_t)count * 8192, false));
    track_lock_slots = false;
    fprintf(stderr, "many guards: slot visits=%llu suspends=%u max batch=%llu\n",
        lock_slot_visits, execution_suspends, largest_slot_batch);
    assert(execution_suspends > 1 && execution_suspends == execution_resumes);
    assert(largest_slot_batch <= 32 && !lock_population_calls);
    assert(mm_locked_bytes(current) == (uint64_t)locked * 4096);
    for (unsigned i = 0; i < count; ++i) {
        struct task_vma *vma = sched_task_vma_at(current, i);
        vma->prot = LINUX_PROT_NONE;
        vma->flags &= ~(TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT);
    }
    reset_population_stats();
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    assert(execution_suspends > 1 && execution_suspends == execution_resumes);
    assert(!lock_population_calls && mm_locked_bytes(current) == (uint64_t)count * 4096);
    teardown();
}

/** @brief A failing file VMA must not suppress a later healthy anonymous VMA. */
static void test_population_local_eio_then_healthy(void)
{
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .size = 16 * 4096, .first_cluster = 7};
    struct vfs_exec_source *source = exec_fixture_open(&node);
    uint64_t file = 0x78000000, healthy = file + 32 * 4096;
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = file, .end = file + node.size, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE,
        .file_node = node, .file_limit = node.size, .exec_source = source};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = healthy, .end = healthy + 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    reset_population_stats(); exec_read_calls = 0; fail_exec_read = true;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    fail_exec_read = false;
    fprintf(stderr, "local EIO: provider calls=%u population calls=%u healthy=%d\n",
        exec_read_calls, lock_population_calls,
        !!address_space_user_page_phys(sched_task_as(current), healthy));
    assert(exec_read_calls == 1 && lock_population_calls == 2);
    assert(!address_space_user_page_phys(sched_task_as(current), file));
    assert(address_space_user_page_phys(sched_task_as(current), healthy));
    assert(mm_locked_bytes(current) == node.size + 4096);
    teardown();
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
}

static uint64_t distant_split_address;
static unsigned nonmutating_peer_visits;
static struct task *unrelated_mm_peer;
static const uint64_t unrelated_mm_address = 0x7f000000;

/** @brief Split an unvisited high-index VMA into already scanned empty slots. */
static void reentry_split_unvisited_vma(void)
{
    execution_reentry = NULL;
    assert(!syscall_mm_mprotect(distant_split_address + 4096, 4096, LINUX_PROT_NONE));
}

/** @brief Read this MM and mutate an independent MM at every suspension. */
static void reentry_nonmutating_peer(void)
{
    ++nonmutating_peer_visits;
    (void)task_vma_containing(current, base, base + 4096);
    struct task *saved = current;
    current = unrelated_mm_peer;
    struct task_vma *vma = task_vma_containing(current, unrelated_mm_address,
                                                unrelated_mm_address + 4096);
    assert(vma);
    uint64_t prot = vma->prot ? LINUX_PROT_NONE : (LINUX_PROT_READ | LINUX_PROT_WRITE);
    assert(!syscall_mm_mprotect(unrelated_mm_address, 4096, prot));
    current = saved;
}

/** @brief Revisit slices moved behind the slot cursor without stale pointers. */
static void test_population_unvisited_split_reentry(void)
{
    setup(1024); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    distant_split_address = 0x7c000000;
    *sched_task_vma_at(current, 700) = (struct task_vma){.used = 1,
        .start = distant_split_address, .end = distant_split_address + 3 * 4096,
        .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_LOCKED};
    reset_population_stats(); track_lock_slots = true;
    execution_reentry = reentry_split_unvisited_vma;
    assert(!mm_populate_locked_range(distant_split_address, distant_split_address + 3 * 4096, false));
    track_lock_slots = false;
    fprintf(stderr, "unvisited split: calls=%u first=%d last=%d max slots=%llu\n",
        lock_population_calls,
        !!address_space_user_page_phys(sched_task_as(current), distant_split_address),
        !!address_space_user_page_phys(sched_task_as(current), distant_split_address + 2 * 4096),
        largest_slot_batch);
    assert(!execution_reentry && execution_suspends > 1 && largest_slot_batch <= 32);
    assert(lock_population_calls == 2);
    assert(address_space_user_page_phys(sched_task_as(current), distant_split_address));
    assert(!address_space_user_page_phys(sched_task_as(current), distant_split_address + 4096));
    assert(address_space_user_page_phys(sched_task_as(current), distant_split_address + 2 * 4096));
    assert(mm_locked_bytes(current) == 3 * 4096);
    teardown();
}

/** @brief Long checked scans progress under repeated unrelated peer activity. */
static void test_checked_long_scan_nonmutating_peer(void)
{
    setup(1024); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    uint64_t address = 0x7e000000;
    *sched_task_vma_at(current, 1000) = (struct task_vma){.used = 1,
        .start = address, .end = address + 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_LOCKED};
    unrelated_mm_peer = calloc(1, sizeof(*unrelated_mm_peer));
    assert(unrelated_mm_peer);
    unrelated_mm_peer->kind = TASK_KIND_USER;
    assert(address_space_create(sched_task_as(unrelated_mm_peer)));
    assert(!mm_context_ensure(sched_task_as(unrelated_mm_peer)));
    *sched_task_vma_at(unrelated_mm_peer, 0) = (struct task_vma){.used = 1,
        .start = unrelated_mm_address, .end = unrelated_mm_address + 4096,
        .prot = 3, .max_prot = 3, .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    reset_population_stats(); track_lock_slots = true;
    nonmutating_peer_visits = 0; execution_reentry = reentry_nonmutating_peer;
    assert(!mm_populate_locked_range(address, address + 4096, true));
    track_lock_slots = false; execution_reentry = NULL;
    fprintf(stderr, "checked long scan: slots=%llu suspends=%u max slots=%llu peer=%u\n",
        lock_slot_visits, execution_suspends, largest_slot_batch, nonmutating_peer_visits);
    assert(nonmutating_peer_visits > 20 && execution_suspends == execution_resumes);
    assert(lock_slot_visits <= 1001 && largest_slot_batch <= 32);
    assert(lock_population_calls == 1 && address_space_user_page_phys(sched_task_as(current), address));
    assert(mm_context_vma_revision(sched_task_as(unrelated_mm_peer)) == nonmutating_peer_visits);
    struct mm_lock_intent intent = {0};
    sched_task_vma_at(current, 1000)->flags &= ~TASK_VMA_FLAG_LOCKED;
    assert(!mm_context_lock_intent_begin(sched_task_as(current), address, address + 4096, &intent));
    uint64_t peer_rev = mm_context_vma_revision(sched_task_as(unrelated_mm_peer));
    reset_population_stats(); track_lock_slots = true;
    nonmutating_peer_visits = 0; execution_reentry = reentry_nonmutating_peer;
    assert(!mm_populate_lock_walk(address, address + 4096, true, false));
    track_lock_slots = false; execution_reentry = NULL;
    fprintf(stderr, "pre-policy long scan: slots=%llu suspends=%u max slots=%llu peer=%u\n",
            lock_slot_visits, execution_suspends, largest_slot_batch, nonmutating_peer_visits);
    assert(nonmutating_peer_visits > 20 && execution_suspends == execution_resumes);
    assert(lock_slot_visits == 1001 && largest_slot_batch <= 32 && lock_population_calls == 1);
    assert(mm_context_vma_revision(sched_task_as(unrelated_mm_peer)) - peer_rev == nonmutating_peer_visits);
    mm_context_lock_intent_end(&intent);
    sched_task_vma_release(unrelated_mm_peer);
    address_space_destroy(sched_task_as(unrelated_mm_peer));
    free(unrelated_mm_peer); unrelated_mm_peer = NULL;
    teardown();
}

/** @brief Track publication, protection, lock, removal and remap mutations. */
static void test_vma_revision_mutation_coverage(void)
{
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    struct address_space *as = sched_task_as(current);
    assert(!mm_context_ensure(as));
    uint64_t rev = mm_context_vma_revision(as), address = 0x80000000;
    uint64_t flags = LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    assert(syscall_mm_mmap(address, 3 * 4096, 3, flags, UINT64_MAX, 0) == (int64_t)address);
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mprotect(address + 4096, 4096, LINUX_PROT_NONE));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlock(address, 4096, LINUX_MLOCK_ONFAULT));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_munlock(address, 4096));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT | LINUX_MCL_ONFAULT));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_munlockall());
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_munmap(address, 4096));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_munmap(address + 16 * 4096, 4096));
    assert(mm_context_vma_revision(as) == rev);
    uint64_t target = address + 32 * 4096;
    assert(syscall_mm_mremap(address + 2 * 4096, 4096, 4096,
        LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, target) == (int64_t)target);
    assert(mm_context_vma_revision(as) > rev);
    teardown();
    puts("PASS MM VMA revision: publication, protection, locking, removal and remap");
}

static const uint64_t noop_peer_address = 0x8a000000;
static struct task *noop_peer;
static unsigned noop_peer_calls, noop_revision_changes;
static bool noop_peer_passed_page32;
static uint64_t noop_peer_frames[100];

/** @brief Issue a same-MM unlock at every simulated prefault suspension. */
static void reentry_same_mm_noop_unlockall(void)
{
    struct task *saved = current;
    current = noop_peer;
    struct address_space *as = sched_task_as(current);
    uint64_t revision = mm_context_vma_revision(as);
    assert(as == sched_task_as(saved));
    assert(!mm_locked_bytes(current) && !mm_lock_flags(as));
    assert(mm_context_lock_intent_intersects(as, noop_peer_address,
                                              noop_peer_address + 100 * 4096));
    for (unsigned i = 0; i < 100; ++i) {
        uint64_t frame = address_space_user_page_phys(as, noop_peer_address + i * 4096);
        if (!frame) continue;
        for (unsigned j = 0; j < i; ++j) assert(noop_peer_frames[j] != frame);
        if (noop_peer_frames[i]) {
            assert(noop_peer_frames[i] == frame);
            assert(*(unsigned char *)mm_phys_to_virt(frame) == (unsigned char)(i + 1));
        } else {
            noop_peer_frames[i] = frame;
            assert(!*(unsigned char *)mm_phys_to_virt(frame));
            *(unsigned char *)mm_phys_to_virt(frame) = (unsigned char)(i + 1);
        }
    }
    if (noop_peer_frames[32]) noop_peer_passed_page32 = true;
    assert(!syscall_mm_munlockall());
    assert(!mm_locked_bytes(current) && !mm_lock_flags(as));
    if (mm_context_vma_revision(as) != revision) ++noop_revision_changes;
    ++noop_peer_calls;
    /* A broken implementation must terminate the fixture rather than hang. */
    if (noop_peer_calls == 5) execution_reentry = NULL;
    current = saved;
}

/** @brief State-preserving same-MM unlock cannot starve checked population. */
static void test_checked_noop_same_mm_peer(void)
{
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(noop_peer_address, 100 * 4096, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)noop_peer_address);
    struct task *parent = current;
    noop_peer = calloc(1, sizeof(*noop_peer));
    struct task_address_space_state *shared = malloc(sizeof(*shared));
    assert(noop_peer && shared);
    *shared = parent->address_space;
    parent->shared_mm = shared;
    noop_peer->shared_mm = shared;
    noop_peer->kind = TASK_KIND_USER;
    memset(noop_peer_frames, 0, sizeof(noop_peer_frames));
    noop_peer_calls = noop_revision_changes = 0;
    noop_peer_passed_page32 = false;
    reset_population_stats();
    execution_reentry = reentry_same_mm_noop_unlockall;
    int64_t result = syscall_mm_mlock(noop_peer_address, 100 * 4096, 0);
    execution_reentry = NULL;
    fprintf(stderr, "same-MM noop peer: result=%lld peers=%u revision_changes=%u "
            "past32=%d population=%u suspends=%u\n", (long long)result,
            noop_peer_calls, noop_revision_changes, noop_peer_passed_page32,
            lock_population_calls, execution_suspends);
    assert(!result && noop_peer_calls >= 3 && !noop_revision_changes);
    assert(noop_peer_passed_page32 && lock_population_calls == 100);
    assert(execution_suspends == execution_resumes && largest_suspend_batch <= 32);
    assert(mm_locked_bytes(parent) == 100 * 4096 && !mm_lock_flags(sched_task_as(parent)));
    assert(sched_task_as(parent)->user_page_count == 100);
    assert(!mm_context_lock_intent_intersects(sched_task_as(parent), noop_peer_address,
                                               noop_peer_address + 100 * 4096));
    for (unsigned i = 0; i < 100; ++i) {
        uint64_t frame = address_space_user_page_phys(sched_task_as(parent), noop_peer_address + i * 4096);
        assert(frame);
        for (unsigned j = 0; j < i; ++j)
            assert(address_space_user_page_phys(sched_task_as(parent), noop_peer_address + j * 4096) != frame);
        if (noop_peer_frames[i])
            assert(noop_peer_frames[i] == frame &&
                   *(unsigned char *)mm_phys_to_virt(frame) == (unsigned char)(i + 1));
        else assert(!*(unsigned char *)mm_phys_to_virt(frame));
        unsigned owner = 0;
        while (owner < 1024 && (!page_refs[owner].refs || page_refs[owner].phys != frame)) ++owner;
        assert(owner < 1024 && page_refs[owner].refs == 1);
    }
    teardown();
    free(noop_peer); noop_peer = NULL; free(shared);
    puts("PASS same-MM noop munlockall preserves revision and checked progress");
}

static const uint64_t current_peer_target = 0x5b000000;
static const uint64_t current_peer_stack = 0x6b000000;
static struct task *current_peer;
static uint64_t current_peer_frames[102];
static unsigned current_peer_refs[102];
static uint32_t current_peer_target_flags, current_peer_stack_flags;
static unsigned current_peer_calls, current_peer_population, current_peer_revision_changes;
static unsigned current_peer_previous_unique, current_peer_stalls;

/** @brief Return the fixture's live reference count for a resident frame.
 * @param frame Physical frame whose owner is tracked by the fixture.
 * @return Number of live physical references; asserts if the frame is untracked.
 */
static unsigned current_peer_frame_refs(uint64_t frame)
{
    for (unsigned i = 0; i < 1024; ++i)
        if (page_refs[i].refs && page_refs[i].phys == frame) return page_refs[i].refs;
    assert(!"resident frame has no tracked owner");
    return 0;
}

/** @brief Check all locked pages and owners around an equal CURRENT peer call. */
static void assert_current_peer_state(void)
{
    struct address_space *as = sched_task_as(current);
    assert(as == sched_task_as(progress_task));
    assert(mm_locked_bytes(current) == 102 * 4096 && !mm_lock_flags(as));
    assert(as->user_page_count == 102);
    struct task_vma *target = task_vma_containing(current, current_peer_target,
                                                  current_peer_target + 100 * 4096);
    struct task_vma *stack = task_vma_containing(current, current_peer_stack,
                                                 current_peer_stack + 2 * 4096);
    assert(target && target->flags == current_peer_target_flags);
    assert(stack && stack->flags == current_peer_stack_flags);
    for (unsigned i = 0; i < 102; ++i) {
        uint64_t page = i < 100 ? current_peer_target + i * 4096 :
                                  current_peer_stack + (i - 100) * 4096;
        uint64_t frame = address_space_user_page_phys(as, page);
        assert(frame && frame == current_peer_frames[i]);
        assert(*(unsigned char *)mm_phys_to_virt(frame) == (unsigned char)(i + 1));
        assert(current_peer_frame_refs(frame) == current_peer_refs[i]);
    }
}

/** @brief Run equal eager CURRENT at each checked walker's simulated release. */
static void reentry_same_mm_eager_current(void)
{
    execution_reentry = NULL; /* Do not recursively inject peers in this peer's walker. */
    if (progress_unique <= current_peer_previous_unique) ++current_peer_stalls;
    current_peer_previous_unique = progress_unique;
    struct task *saved = current;
    current = current_peer;
    struct address_space *as = sched_task_as(current);
    assert(mm_context_lock_intent_intersects(as, current_peer_target,
                                             current_peer_target + 100 * 4096));
    assert_current_peer_state();
    uint64_t revision = mm_context_vma_revision(as);
    unsigned calls = lock_population_calls;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    current_peer_population += lock_population_calls - calls;
    assert_current_peer_state();
    if (mm_context_vma_revision(as) != revision) ++current_peer_revision_changes;
    ++current_peer_calls;
    current = saved;
    /* A broken walker must terminate the RED fixture instead of hanging. */
    if (current_peer_calls < 5) execution_reentry = reentry_same_mm_eager_current;
}

/** @brief An equal eager CURRENT peer cannot restart a checked lock walk. */
static void test_checked_eager_current_same_mm_peer(void)
{
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    current->stack_low = sched_task_mm(current)->initial_stack_low = current_peer_stack;
    current->stack_top = sched_task_mm(current)->initial_stack_top = current_peer_stack + 2 * 4096;
    assert(!task_vma_containing(current, current_peer_stack, current_peer_stack + 4096));
    assert(syscall_mm_mmap(current_peer_target, 100 * 4096, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)current_peer_target);
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    struct task *parent = current;
    struct address_space *as = sched_task_as(parent);
    struct task_vma *target = task_vma_containing(parent, current_peer_target,
                                                  current_peer_target + 100 * 4096);
    struct task_vma *stack = task_vma_containing(parent, current_peer_stack,
                                                 current_peer_stack + 2 * 4096);
    assert(target && stack);
    assert((target->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
           TASK_VMA_FLAG_LOCKED);
    assert((stack->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
           TASK_VMA_FLAG_LOCKED);
    current_peer_target_flags = target->flags;
    current_peer_stack_flags = stack->flags;
    for (unsigned i = 0; i < 102; ++i) {
        uint64_t page = i < 100 ? current_peer_target + i * 4096 :
                                  current_peer_stack + (i - 100) * 4096;
        uint64_t frame = address_space_user_page_phys(as, page);
        assert(frame);
        for (unsigned j = 0; j < i; ++j) assert(current_peer_frames[j] != frame);
        current_peer_frames[i] = frame;
        current_peer_refs[i] = current_peer_frame_refs(frame);
        assert(current_peer_refs[i] == 1);
        *(unsigned char *)mm_phys_to_virt(frame) = (unsigned char)(i + 1);
    }
    current_peer = calloc(1, sizeof(*current_peer));
    struct task_address_space_state *shared = malloc(sizeof(*shared));
    assert(current_peer && shared);
    *shared = parent->address_space;
    parent->shared_mm = shared;
    current_peer->shared_mm = shared;
    as = sched_task_as(parent);
    current_peer->kind = TASK_KIND_USER;
    current_peer->cap_effective = 1ULL << CAP_IPC_LOCK;
    progress_task = parent;
    progress_base = current_peer_target;
    memset(progress_seen, 0, sizeof(progress_seen));
    progress_unique = progress_repeats = 0;
    current_peer_calls = current_peer_population = current_peer_revision_changes = 0;
    current_peer_previous_unique = current_peer_stalls = 0;
    reset_population_stats();
    execution_reentry = reentry_same_mm_eager_current;
    int64_t result = syscall_mm_mlock(current_peer_target, 100 * 4096, 0);
    bool peer_active = execution_reentry == reentry_same_mm_eager_current;
    execution_reentry = NULL;
    fprintf(stderr, "same-MM eager CURRENT: result=%lld peers=%u revision_changes=%u "
            "unique=%u repeats=%u stalls=%u outer_population=%u peer_population=%u active=%d\n",
            (long long)result, current_peer_calls, current_peer_revision_changes,
            progress_unique, progress_repeats, current_peer_stalls,
            lock_population_calls - current_peer_population, current_peer_population, peer_active);
    assert(!result && peer_active && current_peer_calls >= 3 && current_peer_calls < 5);
    assert(!current_peer_revision_changes && !current_peer_stalls && !progress_repeats);
    assert(progress_unique == 100 && lock_population_calls - current_peer_population == 100);
    assert_current_peer_state();
    assert(!mm_context_lock_intent_intersects(as, current_peer_target,
                                               current_peer_target + 100 * 4096));
    progress_task = NULL;
    teardown();
    free(current_peer); current_peer = NULL; free(shared);
    puts("PASS same-MM eager CURRENT preserves state and checked progress");
}

/** @brief Verify implicit-stack admission, best-effort faults and mode transitions. */
static void test_current_implicit_stack_modes(void)
{
    const uint64_t low = 0x6b000000, top = low + 4 * 4096;
    for (unsigned deferred = 0; deferred < 2; ++deferred) {
        setup(1); clear_setup_vmas();
        current->stack_low = sched_task_mm(current)->initial_stack_low = low;
        current->stack_top = sched_task_mm(current)->initial_stack_top = top;
        sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
        sched_task_limits(current)->memlock.rlim_cur = 4 * 4096;
        struct address_space *as = sched_task_as(current);
        assert(!mm_context_ensure(as));
        assert(!task_vma_containing(current, low, low + 4096));
        assert(!syscall_mm_mlock(low + 4096, 4096, LINUX_MLOCK_ONFAULT));
        uint64_t rev = mm_context_vma_revision(as);
        sched_task_limits(current)->memlock.rlim_cur = 0;
        assert(syscall_mm_mlockall(LINUX_MCL_CURRENT) == -RELIEFOS_EPERM);
        assert(mm_context_vma_revision(as) == rev && mm_locked_bytes(current) == 4096);
        sched_task_limits(current)->memlock.rlim_cur = 3 * 4096;
        assert(syscall_mm_mlockall(LINUX_MCL_CURRENT) == -RELIEFOS_ENOMEM);
        assert(mm_context_vma_revision(as) == rev && mm_locked_bytes(current) == 4096);
        assert(!task_vma_containing(current, low, low + 4096));
        sched_task_limits(current)->memlock.rlim_cur = 4 * 4096;
        uint32_t mode = LINUX_MCL_CURRENT | (deferred ? LINUX_MCL_ONFAULT : 0);
        if (!deferred) fail_page = true;
        assert(!syscall_mm_mlockall(mode));
        fail_page = false;
        assert(mm_context_vma_revision(as) > rev && mm_locked_bytes(current) == 4 * 4096);
        for (unsigned i = 0; i < 4; ++i) {
            struct task_vma *vma = task_vma_containing(current, low + i * 4096,
                                                       low + (i + 1) * 4096);
            uint32_t flags = TASK_VMA_FLAG_LOCKED |
                             (deferred ? TASK_VMA_FLAG_LOCKONFAULT : 0);
            assert(vma && (vma->flags & (TASK_VMA_FLAG_LOCKED |
                                         TASK_VMA_FLAG_LOCKONFAULT)) == flags);
            assert(!address_space_user_page_phys(as, low + i * 4096));
        }
        rev = mm_context_vma_revision(as);
        assert(!syscall_mm_mlockall(mode));
        assert(mm_context_vma_revision(as) == rev);
        for (unsigned i = 0; i < 4; ++i)
            assert(!!address_space_user_page_phys(as, low + i * 4096) == !deferred);
        if (deferred) {
            mode = LINUX_MCL_CURRENT | LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT;
            assert(!syscall_mm_mlockall(mode));
            assert(mm_context_vma_revision(as) > rev &&
                   mm_lock_flags(as) == (LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT));
            rev = mm_context_vma_revision(as);
            assert(!syscall_mm_mlockall(mode) && mm_context_vma_revision(as) == rev);
            mode = LINUX_MCL_CURRENT | LINUX_MCL_FUTURE;
            assert(!syscall_mm_mlockall(mode));
            assert(mm_context_vma_revision(as) > rev && mm_lock_flags(as) == LINUX_MCL_FUTURE);
            for (unsigned i = 0; i < 4; ++i) {
                struct task_vma *vma = task_vma_containing(current, low + i * 4096,
                                                           low + (i + 1) * 4096);
                assert(vma && (vma->flags & (TASK_VMA_FLAG_LOCKED |
                                             TASK_VMA_FLAG_LOCKONFAULT)) == TASK_VMA_FLAG_LOCKED);
                assert(address_space_user_page_phys(as, low + i * 4096));
            }
            rev = mm_context_vma_revision(as);
            assert(!syscall_mm_mlockall(mode) && mm_context_vma_revision(as) == rev);
            assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
            assert(mm_context_vma_revision(as) > rev && !mm_lock_flags(as));
            rev = mm_context_vma_revision(as);
            assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT) && mm_context_vma_revision(as) == rev);
            mode = LINUX_MCL_CURRENT | LINUX_MCL_ONFAULT;
            assert(!syscall_mm_mlockall(mode));
            assert(mm_context_vma_revision(as) > rev && mm_lock_flags(as) == LINUX_MCL_ONFAULT);
            for (unsigned i = 0; i < 4; ++i) {
                struct task_vma *vma = task_vma_containing(current, low + i * 4096,
                                                           low + (i + 1) * 4096);
                assert(vma && (vma->flags & (TASK_VMA_FLAG_LOCKED |
                                             TASK_VMA_FLAG_LOCKONFAULT)) ==
                              (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT));
                assert(address_space_user_page_phys(as, low + i * 4096));
            }
            rev = mm_context_vma_revision(as);
            assert(!syscall_mm_mlockall(mode) && mm_context_vma_revision(as) == rev);
        }
        teardown();
    }
    puts("PASS implicit stack coverage, quota and CURRENT/FUTURE transitions");
}

/** @brief Validate no-op policy/protection and real PTE mutation separately. */
static void test_effective_revision_boundaries(void)
{
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x8c000000, length = 4 * 4096;
    assert(syscall_mm_mmap(address, length, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)address);
    struct address_space *as = sched_task_as(current);
    assert(!mm_context_ensure(as));
    struct task_vma *vma = task_vma_containing(current, address, address + length);
    assert(vma);
    vma->max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(syscall_handle_task_page_fault(current, address, 6));
    uint64_t frame = address_space_user_page_phys(as, address);
    *(unsigned char *)mm_phys_to_virt(frame) = 0x5a;
    uint64_t rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mprotect(address, length, 3));
    assert(!syscall_mm_mprotect(address + 4096, 4096, 3));
    assert(mm_context_vma_revision(as) == rev);
    vma = task_vma_containing(current, address, address + length);
    assert(vma && vma->start == address && vma->end == address + length);
    assert(address_space_user_page_phys(as, address) == frame &&
           *(unsigned char *)mm_phys_to_virt(frame) == 0x5a);
    assert(syscall_mm_mprotect(address, length + 4096, 3) == -RELIEFOS_ENOMEM);
    assert(syscall_mm_mprotect(address, length, LINUX_PROT_EXEC) == -RELIEFOS_EACCES);
    assert(!syscall_mm_munlock(address + 4096, 4096));
    assert(mm_context_vma_revision(as) == rev);
    assert(syscall_mm_munlock(address, length + 4096) == -RELIEFOS_ENOMEM);
    assert(!syscall_mm_madvise(address + 4096, 4096, LINUX_MADV_DONTNEED));
    assert(mm_context_vma_revision(as) == rev);
    assert(!syscall_mm_mlock(address, length, LINUX_MLOCK_ONFAULT));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlock(address + 4096, 4096, LINUX_MLOCK_ONFAULT));
    assert(mm_context_vma_revision(as) == rev);
    assert(task_vma_containing(current, address, address + length));
    current->cap_effective = 0;
    sched_task_limits(current)->memlock.rlim_cur = 0;
    assert(syscall_mm_mlock(address, length, LINUX_MLOCK_ONFAULT) == -RELIEFOS_EPERM);
    sched_task_limits(current)->memlock.rlim_cur = 4096;
    assert(syscall_mm_mlock(address, length, LINUX_MLOCK_ONFAULT) == -RELIEFOS_ENOMEM);
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT | LINUX_MCL_ONFAULT));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT | LINUX_MCL_ONFAULT));
    assert(mm_context_vma_revision(as) == rev);
    assert(!syscall_mm_munlockall());
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_munlockall());
    assert(mm_context_vma_revision(as) == rev);
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    assert(mm_context_vma_revision(as) == rev);
    assert(!syscall_mm_munlockall());
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_madvise(address, 4096, LINUX_MADV_DONTNEED));
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!address_space_user_page_phys(as, address));
    frame = mm_alloc_page();
    *(unsigned char *)mm_phys_to_virt(frame) = 0x71;
    assert(address_space_map_user_page(as, address, frame, RELIEFNT_PAGE_NOEXEC));
    assert(!(address_space_user_page_flags(as, address) & RELIEFNT_PAGE_COW));
    assert(!syscall_mm_mprotect(address, 4096, 3));
    assert(address_space_user_page_flags(as, address) & RELIEFNT_PAGE_COW);
    assert(mm_context_vma_revision(as) > rev); rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mprotect(address, 4096, 3));
    assert(mm_context_vma_revision(as) == rev);
    assert(address_space_user_page_phys(as, address) == frame &&
           *(unsigned char *)mm_phys_to_virt(frame) == 0x71);
    fail_page = true;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    fail_page = false;
    assert(mm_locked_bytes(current) == length && as->user_page_count < 4);
    rev = mm_context_vma_revision(as);
    assert(!syscall_mm_mlock(address, length, 0));
    assert(mm_context_vma_revision(as) == rev && as->user_page_count == 4);
    for (unsigned i = 0; i < 4; ++i)
        assert(address_space_user_page_phys(as, address + i * 4096));
    assert(*(unsigned char *)mm_phys_to_virt(address_space_user_page_phys(as, address)) == 0x71);
    teardown();

    setup(1); clear_setup_vmas();
    as = sched_task_as(current);
    assert(!mm_context_ensure(as));
    vma = sched_task_vma_at(current, 0);
    *vma = (struct task_vma){.used = 1, .start = address, .end = address + 2 * 4096,
        .prot = 3, .max_prot = 3, .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_SHARED};
    frame = mm_alloc_page();
    assert(address_space_map_user_page(as, address, frame, RELIEFNT_PAGE_NOEXEC |
                                       RELIEFNT_PAGE_WRITE_NOTIFY | RELIEFNT_PAGE_SHARED));
    rev = mm_context_vma_revision(as);
    allow_shared_write_grant = true; shared_write_grants = 0;
    assert(!syscall_mm_mprotect(address, 4096, 3));
    allow_shared_write_grant = false;
    assert(shared_write_grants == 1 && mm_context_vma_revision(as) == rev);
    assert(address_space_user_page_phys(as, address) == frame && as->user_page_count == 1);
    assert(task_vma_containing(current, address, address + 2 * 4096));
    teardown();
    puts("PASS effective revisions: no-op paths, validation, policy and COW");
}

/** @brief A failed unpublished SysV reservation must not restart a walker. */
static void test_failed_sysv_reservation_revision(void)
{
    setup(1); clear_setup_vmas();
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    struct address_space *as = sched_task_as(current);
    assert(!mm_context_ensure(as));
    const uint64_t address = 0x4000000000ULL;
    uint64_t page = mm_alloc_page();
    uint64_t rev = mm_context_vma_revision(as);
    unsigned before = as->user_page_count;
    struct sysv_shm_attachment *attachment = (struct sysv_shm_attachment *)(uintptr_t)1;
    fail_page = true;
    int64_t ret = syscall_mm_map_sysv_shm(current, address, 4096, 3, 3, false,
                                          attachment, &page);
    fail_page = false;
    fprintf(stderr, "failed SysV preparation: result=%lld revision_delta=%llu leaves=%u\n",
            (long long)ret, (unsigned long long)(mm_context_vma_revision(as) - rev),
            as->user_page_count - before);
    assert(ret == -RELIEFOS_ENOMEM && as->user_page_count == before);
    assert(!task_vma_containing(current, address, address + 4096));
    assert(!address_space_user_page_phys(as, address));
    assert(mm_context_vma_revision(as) == rev);
    assert(syscall_mm_map_sysv_shm(current, address, 4096, 3, 3, false,
                                   attachment, &page) == (int64_t)address);
    assert(mm_context_vma_revision(as) > rev);
    assert(address_space_user_page_phys(as, address) == page);
    assert(!syscall_mm_munmap(address, 4096));
    mm_free_page(page);
    teardown();
    puts("PASS failed SysV reservation preserves revision; publication invalidates");
}

/** @brief An early checked prefault failure must preserve existing lock policy. */
static void test_checked_early_failure_preserves_locks(void)
{
    const uint64_t address = 0x82000000, length = 100 * 4096ULL;
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(address, length, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)address);
    assert(!syscall_mm_mlock(address, 4096, LINUX_MLOCK_ONFAULT));
    reset_population_stats(); fail_page = true;
    assert(syscall_mm_mlock(address, length, 0) == -RELIEFOS_EAGAIN);
    fail_page = false;
    struct task_vma *first = task_vma_containing(current, address, address + 4096);
    struct task_vma *next = task_vma_containing(current, address + 4096, address + 8192);
    assert(first && next);
    assert((first->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
           (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT));
    assert(!(next->flags & TASK_VMA_FLAG_LOCKED));
    assert(mm_locked_bytes(current) == 4096);
    teardown();
    puts("PASS early checked mlock failure preserves prior ONFAULT lock");
}

/** @brief A later split-owner failure must not publish an earlier VMA lock. */
static void test_checked_split_failure_is_atomic(void)
{
    const uint64_t address = 0x83000000;
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = address, .end = address + 2 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE,
        .mapping_file = drm_test_file};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = address + 2 * 4096, .end = address + 4 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE,
        .mapping_file = drm_test_file};
    fail_file_get_after = 1; file_get_calls = file_put_calls = 0;
    assert(syscall_mm_mlock(address + 4096, 2 * 4096, LINUX_MLOCK_ONFAULT) == -RELIEFOS_ENOMEM);
    fail_file_get_after = -1;
    assert(mm_locked_bytes(current) == 0);
    assert(file_get_calls == 2 && file_put_calls == 1);
    struct task_vma *first = task_vma_containing(current, address, address + 8192);
    struct task_vma *second = task_vma_containing(current, address + 2 * 4096, address + 4 * 4096);
    assert(first && second && first->mapping_file == drm_test_file && second->mapping_file == drm_test_file);
    teardown();
    puts("PASS checked mlock stages all split owners before policy publication");
}

/** @brief Unlock and CURRENT stack errors must not partially change policy. */
static void test_unlock_and_current_split_failure_is_atomic(void)
{
    uint64_t address = 0x85000000;
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = address, .end = address + 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_LOCKED};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = address + 4096, .end = address + 3 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE |
                 TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT, .mapping_file = drm_test_file};
    fail_file_get = true;
    assert(syscall_mm_munlock(address, 2 * 4096) == -RELIEFOS_ENOMEM);
    fail_file_get = false;
    assert(mm_locked_bytes(current) == 3 * 4096);
    assert(task_vma_containing(current, address, address + 4096)->flags & TASK_VMA_FLAG_LOCKED);
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    current->stack_low = sched_task_mm(current)->initial_stack_low = address;
    current->stack_top = sched_task_mm(current)->initial_stack_top = address + 3 * 4096;
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = address + 4096, .end = address + 4 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE |
                 TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT, .mapping_file = drm_test_file};
    fail_file_get = true;
    assert(syscall_mm_mlockall(LINUX_MCL_CURRENT | LINUX_MCL_ONFAULT) == -RELIEFOS_ENOMEM);
    fail_file_get = false;
    assert(mm_locked_bytes(current) == 3 * 4096);
    assert(!task_vma_containing(current, address, address + 4096));
    struct task_vma *prior = task_vma_containing(current, address + 4096, address + 4 * 4096);
    assert(prior && (prior->flags & TASK_VMA_FLAG_LOCKONFAULT));
    teardown();
    puts("PASS munlock/CURRENT split failure leaves existing lock policy intact");
}

/** @brief Failure injection bounds retries while preserving policy and refault. */
static void test_best_effort_population_failure_bounds(void)
{
    const uint64_t length = 8192 * 4096ULL, addr = 0x70000000;
    const uint64_t map_flags = LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    setup(1);
    clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    reset_population_stats(); fail_page = true;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    assert(lock_population_calls == 1 && mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr));
    fail_page = false;
    assert(syscall_handle_task_page_fault(current, addr, 6));
    assert(!syscall_mm_munmap(addr, length));

    sched_task_mm(current)->program_break_base = addr;
    sched_task_mm(current)->program_break = addr;
    reset_population_stats(); fail_page = true;
    assert(syscall_mm_brk(addr + length) == (int64_t)(addr + length));
    assert(lock_population_calls == 1 && mm_locked_bytes(current) == length);
    assert(sched_task_mm(current)->program_break == addr + length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr));
    fail_page = false;
    assert(syscall_handle_task_page_fault(current, addr, 6));
    assert(!syscall_mm_munmap(addr, length));

    assert(!syscall_mm_munlockall());
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats(); fail_page = true;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    assert(lock_population_calls == 1 && mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr));
    fail_page = false;
    assert(syscall_handle_task_page_fault(current, addr, 6));
    assert(!syscall_mm_munmap(addr, length));

    assert(syscall_mm_mmap(addr, 4096, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    assert(!syscall_mm_mlock(addr, 4096, 0));
    reset_population_stats(); fail_page = true;
    assert(syscall_mm_mremap(addr, 4096, length, 0, 0) == (int64_t)addr);
    assert(lock_population_calls == 1 && mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr + 4096));
    fail_page = false;
    assert(syscall_handle_task_page_fault(current, addr + 4096, 6));
    teardown();
    puts("PASS mmap/brk/CURRENT/remap population OOM: one attempt, retained policy and later refault");
}

static uint64_t reentry_address;
static struct task_address_space_state *relocated_mm;
static uint64_t reentry_break;
static uint64_t reentry_old;

/** @brief Relocate inline MM state and revoke one not-yet-populated page. */
static void reentry_relocate_and_protect(void)
{
    execution_reentry = NULL;
    relocated_mm = malloc(sizeof(*relocated_mm));
    assert(relocated_mm && !current->shared_mm);
    *relocated_mm = current->address_space;
    current->shared_mm = relocated_mm;
    memset(&current->address_space, 0, sizeof(current->address_space));
    assert(!syscall_mm_mprotect(reentry_address + 32 * 4096, 4096, LINUX_PROT_NONE));
}

/** @brief Remove the next page while a checked mlock drops execution. */
static void reentry_unmap_next(void)
{
    execution_reentry = NULL;
    assert(!syscall_mm_munmap(reentry_address + 32 * 4096, 4096));
}

/** @brief Force a later checked allocation failure at the first batch boundary. */
static void reentry_fail_next_allocation(void)
{
    execution_reentry = NULL;
    fail_page = true;
}

/** @brief Inject allocation failure after at least forty checked page attempts. */
static void reentry_fail_later_allocation(void)
{
    if (lock_population_calls < 40) return;
    execution_reentry = NULL;
    fail_page = true;
}

/** @brief A late checked fault failure preserves an earlier ONFAULT lock. */
static void test_checked_late_failure_preserves_locks(void)
{
    const uint64_t address = 0x84000000, length = 100 * 4096ULL;
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(address, length, 3,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)address);
    assert(!syscall_mm_mlock(address + 64 * 4096, 4096, LINUX_MLOCK_ONFAULT));
    reset_population_stats(); execution_reentry = reentry_fail_later_allocation;
    assert(syscall_mm_mlock(address, length, 0) == -RELIEFOS_EAGAIN);
    fail_page = false;
    struct task_vma *first = task_vma_containing(current, address, address + 4096);
    struct task_vma *prior = task_vma_containing(current, address + 64 * 4096,
                                                  address + 65 * 4096);
    assert(first && prior && !(first->flags & TASK_VMA_FLAG_LOCKED));
    assert((prior->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
           (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT));
    assert(mm_locked_bytes(current) == 4096);
    assert(lock_population_calls >= 40 && !execution_reentry);
    assert(address_space_user_page_phys(sched_task_as(current), address));
    teardown();
    puts("PASS late checked mlock failure preserves prior ONFAULT lock");
}

static uint64_t intent_test_address;
static bool intent_discard_seen;

/** @brief A peer can lock its own page and then force the original fault to fail. */
static void reentry_peer_lock_and_fail(void)
{
    execution_reentry = NULL;
    assert(mm_context_lock_intent_intersects(sched_task_as(current), intent_test_address,
                                              intent_test_address + 100 * 4096));
    assert(!syscall_mm_mlock(intent_test_address + 80 * 4096, 4096, LINUX_MLOCK_ONFAULT));
    fail_page = true;
}

/** @brief A peer discard forces a checked prefault restart before publication. */
static void reentry_discard_prefaulted_page(void)
{
    execution_reentry = NULL;
    assert(mm_context_lock_intent_intersects(sched_task_as(current), intent_test_address,
                                              intent_test_address + 4096));
    assert(!mm_context_lock_intent_intersects(sched_task_as(current), intent_test_address - 4096,
                                               intent_test_address));
    assert(mm_locked_bytes(current) == 4096);
    assert(address_space_user_page_phys(sched_task_as(current), intent_test_address));
    uint64_t rev = mm_context_vma_revision(sched_task_as(current));
    assert(!syscall_mm_madvise(intent_test_address, 4096, LINUX_MADV_DONTNEED));
    assert(mm_context_vma_revision(sched_task_as(current)) > rev);
    assert(!address_space_user_page_phys(sched_task_as(current), intent_test_address));
    intent_discard_seen = true;
}

/** @brief A concurrent limit change must be rechecked before publication. */
static void reentry_reduce_lock_limit(void)
{
    execution_reentry = NULL;
    assert(mm_context_lock_intent_intersects(sched_task_as(current), intent_test_address,
                                              intent_test_address + 4096));
    sched_task_limits(current)->memlock.rlim_cur = 4096;
}

/** @brief Peer policy survives error; discarded pages refault before success. */
static void test_checked_intent_reentry(void)
{
    const uint64_t length = 100 * 4096ULL;
    intent_test_address = 0x86000000;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        setup(1); clear_setup_vmas();
        current->cap_effective = scenario == 2 ? 0 : (1ULL << CAP_IPC_LOCK);
        sched_task_limits(current)->memlock.rlim_cur = length;
        sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
        assert(syscall_mm_mmap(intent_test_address, length, 3,
            LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
            UINT64_MAX, 0) == (int64_t)intent_test_address);
        assert(!syscall_mm_mlock(intent_test_address + 64 * 4096, 4096, LINUX_MLOCK_ONFAULT));
        reset_population_stats(); intent_discard_seen = false;
        execution_reentry = scenario == 0 ? reentry_peer_lock_and_fail :
                            scenario == 1 ? reentry_discard_prefaulted_page : reentry_reduce_lock_limit;
        int64_t ret = syscall_mm_mlock(intent_test_address, length, 0);
        fail_page = false;
        assert(!execution_reentry);
        assert(!mm_context_lock_intent_intersects(sched_task_as(current), intent_test_address,
                                                   intent_test_address + length));
        if (!scenario) {
            assert(ret == -RELIEFOS_EAGAIN && mm_locked_bytes(current) == 2 * 4096);
            for (unsigned i = 64; i <= 80; i += 16) {
                struct task_vma *v = task_vma_containing(current, intent_test_address + i * 4096,
                                                          intent_test_address + (i + 1) * 4096);
                assert(v && (v->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
                            (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT));
            }
        } else if (scenario == 1) {
            assert(!ret && intent_discard_seen && mm_locked_bytes(current) == length);
            for (unsigned i = 0; i < 100; ++i)
                assert(address_space_user_page_phys(sched_task_as(current), intent_test_address + i * 4096));
        } else {
            assert(ret == -RELIEFOS_ENOMEM && mm_locked_bytes(current) == 4096);
            assert(address_space_user_page_phys(sched_task_as(current), intent_test_address + 99 * 4096));
            struct task_vma *prior = task_vma_containing(current, intent_test_address + 64 * 4096,
                                                          intent_test_address + 65 * 4096);
            assert(prior && (prior->flags & TASK_VMA_FLAG_LOCKONFAULT));
        }
        teardown();
    }
    puts("PASS checked intent: peer lock, discard/refault, full residency and quota revalidation");
}

/** @brief Checked file-provider errors before/after anonymous faults keep policy. */
static void test_checked_provider_failure_preserves_locks(void)
{
    for (unsigned late = 0; late < 2; ++late) {
        setup(1); clear_setup_vmas();
        current->cap_effective = 1ULL << CAP_IPC_LOCK;
        uint64_t address = 0x88000000, anon = address + (late ? 0 : 4096);
        uint64_t file = late ? address + 64 * 4096 : address;
        struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .size = 4096, .first_cluster = 9};
        struct vfs_exec_source *source = exec_fixture_open(&node);
        *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
            .start = anon, .end = anon + 64 * 4096, .prot = 3, .max_prot = 3,
            .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
        *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
            .start = file, .end = file + 4096, .prot = 3, .max_prot = 3,
            .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE,
            .file_node = node, .file_limit = 4096, .exec_source = source};
        assert(!syscall_mm_mlock(anon + 63 * 4096, 4096, LINUX_MLOCK_ONFAULT));
        reset_population_stats(); exec_read_calls = 0; fail_exec_read = true;
        assert(syscall_mm_mlock(address, 65 * 4096, 0) == -RELIEFOS_ENOMEM);
        fail_exec_read = false;
        assert(exec_read_calls == 1 && mm_locked_bytes(current) == 4096);
        struct task_vma *prior = task_vma_containing(current, anon + 63 * 4096, anon + 64 * 4096);
        assert(prior && (prior->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT)) ==
                        (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT));
        assert(!mm_context_lock_intent_intersects(sched_task_as(current), address, address + 65 * 4096));
        if (late) assert(lock_population_calls > 64 && address_space_user_page_phys(sched_task_as(current), anon));
        else assert(lock_population_calls == 1 && !address_space_user_page_phys(sched_task_as(current), anon));
        teardown();
        assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
    }
    puts("PASS early/late file provider errors leave checked lock policy unchanged");
}

/** @brief Observe a committed brk or moved mapping before relocating its MM. */
static void reentry_committed_and_relocate(void)
{
    if (reentry_break) assert(sched_task_mm(current)->program_break == reentry_break);
    if (reentry_old) assert(!task_vma_containing(current, reentry_old, reentry_old + 4096));
    reentry_relocate_and_protect();
}

/** @brief Unmap an already populated shared prefix during a best-effort walk. */
static void reentry_unmap_shared_prefix(void)
{
    execution_reentry = NULL;
    assert(!syscall_mm_munmap(reentry_address, 32 * 4096));
}

/** @brief Reentry resolves relocated MM/changed VMAs and bounds successful work. */
static void test_population_cooperative_reentry(void)
{
    const uint64_t addr = 0x74000000, length = 100 * 4096ULL;
    const uint64_t map_flags = LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    reset_population_stats(); reentry_address = addr;
    execution_reentry = reentry_relocate_and_protect;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    assert(execution_suspends >= 3 && execution_resumes == execution_suspends);
    assert(largest_suspend_batch <= 32 && lock_population_calls >= 99 && lock_population_calls <= 131);
    assert(mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr + 32 * 4096));
    assert(address_space_user_page_phys(sched_task_as(current), addr + 33 * 4096));
    assert(!syscall_mm_mprotect(addr + 32 * 4096, 4096, 3));
    assert(syscall_handle_task_page_fault(current, addr + 32 * 4096, 6));
    teardown(); free(relocated_mm); relocated_mm = NULL;

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats(); reentry_address = addr;
    execution_reentry = reentry_relocate_and_protect;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    assert(execution_suspends >= 3 && execution_resumes == execution_suspends);
    assert(largest_suspend_batch <= 32 && lock_population_calls >= 99 && lock_population_calls <= 131);
    assert(mm_locked_bytes(current) == length);
    assert(!address_space_user_page_phys(sched_task_as(current), addr + 32 * 4096));
    assert(address_space_user_page_phys(sched_task_as(current), addr + 99 * 4096));
    teardown(); free(relocated_mm); relocated_mm = NULL;

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats(); reentry_address = addr; execution_reentry = reentry_unmap_next;
    assert(syscall_mm_mlock(addr, length, 0) == -RELIEFOS_ENOMEM);
    assert(execution_suspends >= 1 && execution_resumes == execution_suspends);
    fprintf(stderr, "checked unmap reentry: calls=%u max batch=%u suspends=%u\n",
            lock_population_calls, largest_suspend_batch, execution_suspends);
    assert(largest_suspend_batch <= 32 && lock_population_calls == 65);
    assert(mm_locked_bytes(current) == 0);
    assert(!address_space_user_page_phys(sched_task_as(current), addr + 32 * 4096));
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats();
    assert(!syscall_mm_mlock(addr, length, 0));
    assert(lock_population_calls == 100 && execution_suspends >= 3);
    assert(execution_resumes == execution_suspends && largest_suspend_batch <= 32);
    assert(mm_locked_bytes(current) == length);
    assert(address_space_user_page_phys(sched_task_as(current), addr + 99 * 4096));
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats(); execution_reentry = reentry_fail_next_allocation;
    assert(syscall_mm_mlock(addr, length, 0) == -RELIEFOS_EAGAIN);
    fail_page = false;
    assert(lock_population_calls == 33 && execution_suspends == 1);
    assert(mm_locked_bytes(current) == 0);
    assert(!address_space_user_page_phys(sched_task_as(current), addr + 32 * 4096));
    assert(syscall_handle_task_page_fault(current, addr + 32 * 4096, 6));
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mmap(addr, length, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    reset_population_stats(); execution_suspend_fails = true;
    assert(syscall_mm_mlock(addr, length, 0) == -RELIEFOS_EAGAIN);
    assert(lock_population_calls == 32 && !execution_suspends);
    assert(mm_locked_bytes(current) == 0);
    execution_suspend_fails = false;
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    sched_task_mm(current)->program_break_base = addr;
    sched_task_mm(current)->program_break = addr;
    reset_population_stats(); reentry_address = addr; reentry_break = addr + length;
    execution_reentry = reentry_committed_and_relocate;
    assert(syscall_mm_brk(reentry_break) == (int64_t)reentry_break);
    assert(execution_suspends >= 3 && sched_task_mm(current)->program_break == reentry_break);
    assert(mm_locked_bytes(current) == length && largest_suspend_batch <= 32);
    teardown(); free(relocated_mm); relocated_mm = NULL; reentry_break = 0;

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    reset_population_stats(); reentry_address = addr;
    execution_reentry = reentry_unmap_shared_prefix;
    assert(syscall_mm_mmap(addr, length, 3,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
        UINT64_MAX, 0) == (int64_t)addr);
    assert(execution_suspends >= 3 && mm_locked_bytes(current) == length - 32 * 4096);
    assert(!mm_shared_resident(sched_task_as(current), addr));
    assert(mm_shared_resident(sched_task_as(current), addr + 32 * 4096));
    assert(!address_space_user_page_phys(sched_task_as(current), addr));
    assert(address_space_user_page_phys(sched_task_as(current), addr + 99 * 4096));
    teardown();

    setup(1); clear_setup_vmas();
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t target = addr + 0x200000;
    assert(syscall_mm_mmap(addr, 4096, 3, map_flags, UINT64_MAX, 0) == (int64_t)addr);
    assert(!syscall_mm_mlock(addr, 4096, 0));
    reset_population_stats(); reentry_address = target; reentry_old = addr;
    execution_reentry = reentry_committed_and_relocate;
    assert(syscall_mm_mremap(addr, 4096, length,
        LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, target) == (int64_t)target);
    assert(execution_suspends >= 3 && mm_locked_bytes(current) == length);
    assert(largest_suspend_batch <= 32);
    assert(!address_space_user_page_phys(sched_task_as(current), target + 32 * 4096));
    teardown(); free(relocated_mm); relocated_mm = NULL; reentry_old = 0;
    puts("PASS 32-page cooperative boundaries: MM relocation, VMA changes, shared owners and checked mlock results");
}

/** @brief Verify discarded private data is retired before free and refaults zero. */
static void test_dontneed_private_retirement(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(syscall_handle_private_anon_fault(current, base, 6));
    retired_probe_page = address_space_user_page_phys(sched_task_as(current), base);
    assert(retired_probe_page);
    *(unsigned char *)mm_phys_to_virt(retired_probe_page) = 0x93;
    retired_probe_flushed = false;
    assert(syscall_mm_madvise(base, 4096, LINUX_MADV_DONTNEED) == 0);
    assert(retired_probe_flushed);
    retired_probe_page = 0;
    assert(!address_space_user_page_phys(sched_task_as(current), base));
    assert(syscall_handle_private_anon_fault(current, base, 6));
    uint64_t replacement = address_space_user_page_phys(sched_task_as(current), base);
    assert(replacement && *(unsigned char *)mm_phys_to_virt(replacement) == 0);
    teardown();
}

/** @brief Lock an unaligned partial interval without charging its neighbors. */
static void test_mlock_partial_onfault_and_quota(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    sched_task_limits(current)->memlock.rlim_cur = 2 * 4096;
    assert(syscall_mm_mlock(base + 1, 4096, LINUX_MLOCK_ONFAULT) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        vma = task_vma_containing(current, address, address + 4096);
        assert(vma && !!(vma->flags & TASK_VMA_FLAG_LOCKED) == (i < 2));
        assert(!address_space_user_page_phys(sched_task_as(current), address));
    }
    assert(syscall_mm_mlock(base, 2 * 4096, LINUX_MLOCK_ONFAULT) == 0);
    assert(syscall_mm_mlock(base + 2 * 4096, 4096, LINUX_MLOCK_ONFAULT) == -RELIEFOS_ENOMEM);
    teardown();
}

/** @brief Lock and discard a genuine initial-stack page without an initial VMA. */
static void test_mlock_implicit_initial_stack(void)
{
    setup(1);
    uint64_t top = RELIEFNT_USER_TOP - 4096;
    uint64_t low = top - 3 * 4096;
    current->stack_top = top;
    current->stack_low = low;
    sched_task_mm(current)->initial_stack_top = top;
    sched_task_mm(current)->initial_stack_low = low;
    sched_task_limits(current)->memlock.rlim_cur = 3 * 4096;
    assert(!task_vma_containing(current, low, low + 4096));
    assert(address_space_map_user_stack_page(sched_task_as(current), low));
    assert(syscall_mm_mlock(low + 1, 1, LINUX_MLOCK_ONFAULT) == 0);
    assert(mm_locked_bytes(current) == 4096);
    assert(syscall_mm_madvise(low, 4096, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    assert(syscall_mm_munlock(low, 4096) == 0);
    assert(mm_locked_bytes(current) == 0);
    assert(syscall_mm_madvise(low, 4096, LINUX_MADV_DONTNEED) == 0);
    assert(!address_space_user_page_phys(sched_task_as(current), low));
    assert(!syscall_mm_mlock(low, 3 * 4096, 0));
    assert(mm_locked_bytes(current) == 3 * 4096);
    for (uint64_t page = low; page < top; page += 4096)
        assert(address_space_user_page_phys(sched_task_as(current), page));
    teardown();
}

/** @brief A peer using the same MM observes future-lock and ONFAULT policy. */
static void test_mlockall_shared_mm_policy(void)
{
    setup(1);
    struct task *parent = current;
    struct task *peer = calloc(1, sizeof(*peer));
    struct task_address_space_state *shared = malloc(sizeof(*shared));
    assert(peer && shared);
    struct mm_lock_intent intent = {0};
    assert(!mm_context_lock_intent_begin(sched_task_as(parent), base, base + 4096, &intent));
    uint64_t revision = mm_context_vma_revision(sched_task_as(parent));
    *shared = parent->address_space;
    parent->shared_mm = shared;
    peer->shared_mm = shared;
    peer->kind = TASK_KIND_USER;
    assert(mm_context_lock_intent_intersects(sched_task_as(peer), base, base + 4096));
    assert(mm_context_vma_revision(sched_task_as(peer)) == revision);
    parent->limits.memlock.rlim_cur = peer->limits.memlock.rlim_cur = 4096;
    peer->limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    assert(syscall_mm_mlockall((1ULL << 32) | LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT) == 0);
    current = peer;
    uint64_t address = base + 0x1000000;
    assert(syscall_mm_mmap(address, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE,
           LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    struct task_vma *vma = task_vma_containing(peer, address, address + 4096);
    assert(vma && (vma->flags & TASK_VMA_FLAG_LOCKED));
    assert(vma->flags & TASK_VMA_FLAG_LOCKONFAULT);
    assert(!address_space_user_page_phys(sched_task_as(peer), address));
    current = parent;
    mm_context_lock_intent_end(&intent);
    teardown();
    free(peer);
    free(shared);
}

/** @brief Eager locks populate private pages, break COW and report failures. */
static void test_mlock_prefault(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    sched_task_limits(current)->memlock.rlim_cur = 5 * 4096;
    assert(syscall_handle_private_anon_fault(current, base, 6));
    uint64_t original = address_space_user_page_phys(sched_task_as(current), base);
    *(unsigned char *)mm_phys_to_virt(original) = 0x71;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    assert(syscall_mm_mlock(base, 4096, 0) == 0);
    uint64_t private_page = address_space_user_page_phys(sched_task_as(current), base);
    assert(private_page && private_page != original);
    assert(address_space_user_page_writable(sched_task_as(current), base));
    assert(*(unsigned char *)mm_phys_to_virt(private_page) == 0x71);
    assert(address_space_user_page_phys(&child, base) == original);
    address_space_destroy(&child);

    assert(syscall_mm_mprotect(base + 4096, 4096, LINUX_PROT_NONE) == 0);
    assert(syscall_mm_mlock(base + 4096, 4096, 0) == -RELIEFOS_ENOMEM);
    assert(!address_space_user_page_phys(sched_task_as(current), base + 4096));
    assert(syscall_mm_mlock(base + 4096, 4096, LINUX_MLOCK_ONFAULT) == 0);

    fail_page = true;
    assert(syscall_mm_mlock(base + 2 * 4096, 4096, 0) == -RELIEFOS_EAGAIN);
    fail_page = false;
    vma = task_vma_containing(current, base + 2 * 4096, base + 3 * 4096);
    assert(vma && !(vma->flags & TASK_VMA_FLAG_LOCKED));
    teardown();
}

/** @brief CURRENT charges and populates ordinary VMAs and the implicit stack. */
static void test_mlockall_current_stack_and_quota(void)
{
    setup(1);
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = base, .end = base + 2 * 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    uint64_t top = RELIEFNT_USER_TOP - 4096;
    current->stack_top = top;
    current->stack_low = top - 2 * 4096;
    sched_task_mm(current)->initial_stack_top = top;
    sched_task_mm(current)->initial_stack_low = top - 2 * 4096;
    sched_task_limits(current)->memlock.rlim_cur = 3 * 4096;
    assert(syscall_mm_mlockall(LINUX_MCL_CURRENT) == -RELIEFOS_ENOMEM);
    assert(mm_locked_bytes(current) == 0);
    sched_task_limits(current)->memlock.rlim_cur = 4 * 4096;
    assert(syscall_mm_mlockall(LINUX_MCL_CURRENT) == 0);
    assert(mm_locked_bytes(current) == 4 * 4096);
    for (unsigned i = 0; i < 2; ++i) {
        assert(address_space_user_page_phys(sched_task_as(current), base + i * 4096));
        assert(address_space_user_page_phys(sched_task_as(current), top - (i + 1) * 4096));
    }
    assert(syscall_mm_munlockall() == 0);
    assert(mm_locked_bytes(current) == 0);
    teardown();
}

/** @brief CURRENT admits guards and preserves policy when population fails. */
static void test_mlockall_current_guard_and_oom(void)
{
    setup(1);
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1,
        .start = base, .end = base + 4096, .prot = LINUX_PROT_NONE,
        .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = base + 4096, .end = base + 8192,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    sched_task_limits(current)->memlock.rlim_cur = 4096;
    assert(syscall_mm_mlockall(LINUX_MCL_CURRENT) == -RELIEFOS_ENOMEM);
    assert(!mm_locked_bytes(current));
    sched_task_limits(current)->memlock.rlim_cur = 8192;
    fail_page = true;
    assert(!syscall_mm_mlockall(LINUX_MCL_CURRENT));
    fail_page = false;
    assert(mm_locked_bytes(current) == 8192);
    assert(!address_space_user_page_phys(sched_task_as(current), base));
    assert(!address_space_user_page_phys(sched_task_as(current), base + 4096));
    assert(!syscall_mm_mprotect(base, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE));
    assert(syscall_handle_task_page_fault(current, base, 6));
    assert(syscall_handle_task_page_fault(current, base + 4096, 6));
    assert(mm_locked_bytes(current) == 8192);
    teardown();
}

/** @brief Shared anonymous discard removes the PTE but preserves shared data. */
static void test_dontneed_shared_anonymous(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t address = base + 0x1000000;
    assert(syscall_mm_mmap(address, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    assert(syscall_handle_task_page_fault(current, address, 6));
    uint64_t original = address_space_user_page_phys(sched_task_as(current), address);
    assert(original);
    *(unsigned char *)mm_phys_to_virt(original) = 0x6d;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    assert(syscall_mm_madvise(address, 4096, LINUX_MADV_DONTNEED) == 0);
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(address_space_user_page_phys(&child, address) == original);
    mincore_bytes[0] = 0;
    assert(syscall_mm_mincore(address, 4096, (uintptr_t)mincore_bytes) == 0);
    assert(mincore_bytes[0] == 1);
    assert(syscall_handle_task_page_fault(current, address, 6));
    uint64_t restored = address_space_user_page_phys(sched_task_as(current), address);
    assert(restored == original && *(unsigned char *)mm_phys_to_virt(restored) == 0x6d);
    address_space_destroy(&child);
    teardown();
}

/** @brief Shared faults require owned backing and survive metadata exhaustion. */
static void test_shared_fault_ownership_failure(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_SHARED;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(!syscall_handle_task_page_fault(current, base, 6));
    assert(!address_space_user_page_phys(sched_task_as(current), base));
    teardown();

    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t address = base + 0x1000000;
    assert(syscall_mm_mmap(address, 4097 * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    assert(syscall_handle_task_page_fault(current, address, 6));
    uint64_t original = address_space_user_page_phys(sched_task_as(current), address);
    *(uint8_t *)mm_phys_to_virt(original) = 0x5a;
    unsigned before = pages;
    fail_after = 0;
    assert(!syscall_handle_task_page_fault(current, address + 64 * 4096, 6));
    assert(!address_space_user_page_phys(sched_task_as(current), address + 64 * 4096));
    assert(pages == before);
    unsigned live_before = allocations - deallocations;
    fail_after = 1;
    assert(!syscall_handle_task_page_fault(current, address + 4096 * 4096, 6));
    assert(!address_space_user_page_phys(sched_task_as(current), address + 4096 * 4096));
    assert(pages == before && allocations - deallocations == live_before);
    fail_after = -1;
    assert(!syscall_mm_madvise(address, 4096, LINUX_MADV_DONTNEED));
    assert(syscall_handle_task_page_fault(current, address, 6));
    assert(address_space_user_page_phys(sched_task_as(current), address) == original);
    assert(*(uint8_t *)mm_phys_to_virt(original) == 0x5a);
    assert(syscall_handle_task_page_fault(current, address + 64 * 4096, 6));
    teardown();
}

/** @brief mincore reports backing residency even for a PROT_NONE PTE. */
static void test_mincore_residency_and_bounds(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(syscall_handle_private_anon_fault(current, base, 6));
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_NONE) == 0);
    mincore_bytes[0] = mincore_bytes[1] = 0xff;
    assert(syscall_mm_mincore(base, 2 * 4096, (uintptr_t)mincore_bytes) == 0);
    assert(mincore_bytes[0] == 1 && mincore_bytes[1] == 0);
    assert(syscall_mm_mincore(base + 1, 4096, (uintptr_t)mincore_bytes) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mincore(base, 4096, 1) == -RELIEFOS_EFAULT);
    assert(syscall_mm_mincore(base, 0, 1) == 0);
    assert(syscall_mm_mincore(RELIEFNT_USER_TOP - 4096, 8192,
                              (uintptr_t)mincore_bytes) == -RELIEFOS_ENOMEM);
    teardown();
}

/** @brief DONTNEED visits mapped pieces around holes and validates arguments. */
static void test_dontneed_holes_and_rejections(void)
{
    setup(1);
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1, .start = base,
        .end = base + 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = base + 2 * 4096, .end = base + 3 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    assert(syscall_handle_private_anon_fault(current, base, 6));
    assert(syscall_handle_private_anon_fault(current, base + 2 * 4096, 6));
    assert(syscall_mm_madvise(base, 3 * 4096, LINUX_MADV_DONTNEED) == -RELIEFOS_ENOMEM);
    assert(!address_space_user_page_phys(sched_task_as(current), base));
    assert(!address_space_user_page_phys(sched_task_as(current), base + 2 * 4096));
    assert(syscall_mm_madvise(base + 1, 0, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    assert(syscall_mm_madvise(base, 0, 0x7fffffff) == -RELIEFOS_EINVAL);
    assert(syscall_mm_madvise(base, 0, (1ULL << 32) | LINUX_MADV_DONTNEED) == 0);
    assert(syscall_mm_madvise(base, UINT64_MAX, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    sched_task_vma_at(current, 0)->flags |= TASK_VMA_FLAG_DEVICE;
    assert(syscall_mm_madvise(base, 4096, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    teardown();
}

/** @brief FUTURE locks new stack growth but not refaults of explicitly unlocked pages. */
static void test_mlockall_future_stack_growth(void)
{
    setup(1);
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
    uint64_t top = RELIEFNT_USER_TOP - 4096;
    current->stack_top = top;
    current->stack_low = top - 4096;
    sched_task_mm(current)->initial_stack_top = top;
    sched_task_mm(current)->initial_stack_low = top - 4096;
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    sched_task_limits(current)->stack.rlim_cur = 8 * 4096;
    sched_task_limits(current)->memlock.rlim_cur = 2 * 4096;
    assert(address_space_map_user_stack_page(sched_task_as(current), top - 4096));
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT));
    uint64_t page = top - 2 * 4096;
    assert(syscall_handle_task_page_fault(current, page, 6));
    assert(mm_locked_bytes(current) == 4096);
    assert(!syscall_mm_munlock(page, 4096));
    assert(!syscall_mm_madvise(page, 4096, LINUX_MADV_DONTNEED));
    assert(syscall_handle_task_page_fault(current, page, 6));
    assert(mm_locked_bytes(current) == 0);
    assert(syscall_handle_task_page_fault(current, page - 4096, 6));
    assert(mm_locked_bytes(current) == 4096);
    teardown();
}

/** @brief A shared-anonymous move keeps original backing for both mapped and discarded pages. */
static void test_shared_anonymous_mremap(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t old = 0x50000000, moved = 0x52000000;
    void *host_old = mmap((void *)(uintptr_t)old, 8192, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *host_moved = mmap((void *)(uintptr_t)moved, 12288, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(host_old == (void *)(uintptr_t)old && host_moved == (void *)(uintptr_t)moved);
    assert(syscall_mm_mmap(old, 8192, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)old);
    assert(syscall_handle_task_page_fault(current, old, 6));
    assert(syscall_handle_task_page_fault(current, old + 4096, 6));
    uint64_t p0 = address_space_user_page_phys(sched_task_as(current), old);
    uint64_t p1 = address_space_user_page_phys(sched_task_as(current), old + 4096);
    *(uint8_t *)mm_phys_to_virt(p0) = 0x41;
    *(uint8_t *)mm_phys_to_virt(p1) = 0x82;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    assert(!syscall_mm_madvise(old + 4096, 4096, LINUX_MADV_DONTNEED));
    assert(syscall_mm_mremap(old, 8192, 12288,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, moved) == (int64_t)moved);
    assert(!address_space_user_page_phys(sched_task_as(current), old));
    assert(address_space_user_page_phys(sched_task_as(current), moved) == p0);
    assert(!address_space_user_page_phys(sched_task_as(current), moved + 4096));
    assert(syscall_handle_task_page_fault(current, moved + 4096, 6));
    assert(address_space_user_page_phys(sched_task_as(current), moved + 4096) == p1);
    assert(*(uint8_t *)mm_phys_to_virt(p1) == 0x82);
    assert(address_space_user_page_phys(&child, old) == p0);
    assert(address_space_user_page_phys(&child, old + 4096) == p1);
    address_space_destroy(&child);
    teardown();
    assert(!munmap(host_old, 8192));
    assert(!munmap(host_moved, 12288));
}

/** @brief Move private inaccessible, read-only and COW leaves without user copies. */
static void test_private_locked_fixed_mremap(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    sched_task_limits(current)->memlock.rlim_cur = 8 * 4096;
    const uint64_t src = 0x60000000, dst = 0x64000000;
    uint32_t flags = LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    assert(syscall_mm_mmap(src, 4096, LINUX_PROT_NONE, flags, UINT64_MAX, 0) == (int64_t)src);
    assert(!syscall_mm_mlock(src, 4096, LINUX_MLOCK_ONFAULT));
    assert(syscall_mm_mremap(src, 4096, 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, dst) == (int64_t)dst);
    assert(!address_space_user_page_phys(sched_task_as(current), dst));
    assert(task_vma_containing(current, dst, dst + 4096)->flags & TASK_VMA_FLAG_LOCKONFAULT);
    assert(!syscall_mm_munmap(dst, 4096));

    assert(syscall_mm_mmap(src, 4096, 3, flags, UINT64_MAX, 0) == (int64_t)src);
    assert(syscall_handle_task_page_fault(current, src, 6));
    uint64_t hidden = address_space_user_page_phys(sched_task_as(current), src);
    *(uint8_t *)mm_phys_to_virt(hidden) = 0x72;
    assert(!syscall_mm_mprotect(src, 4096, LINUX_PROT_NONE));
    assert(!syscall_mm_mlock(src, 4096, LINUX_MLOCK_ONFAULT));
    assert(syscall_mm_mremap(src, 4096, 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, dst) == (int64_t)dst);
    assert(address_space_user_page_phys(sched_task_as(current), dst) == hidden);
    assert(address_space_user_page_flags(sched_task_as(current), dst) & RELIEFNT_PAGE_PROTNONE);
    assert(!address_space_user_page_readable(sched_task_as(current), dst));
    assert(!syscall_mm_mprotect(dst, 4096, LINUX_PROT_READ));
    assert(*(uint8_t *)mm_phys_to_virt(hidden) == 0x72);
    assert(!syscall_mm_munmap(dst, 4096));

    assert(syscall_mm_mmap(src, 8192, LINUX_PROT_READ, flags, UINT64_MAX, 0) == (int64_t)src);
    assert(syscall_handle_task_page_fault(current, src, 4));
    uint64_t original = address_space_user_page_phys(sched_task_as(current), src);
    *(uint8_t *)mm_phys_to_virt(original) = 0x83;
    assert(!syscall_mm_mlock(src, 8192, LINUX_MLOCK_ONFAULT));
    assert(syscall_mm_mremap(src, 8192, 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, dst) == (int64_t)dst);
    assert(address_space_user_page_phys(sched_task_as(current), dst) == original);
    assert(!address_space_user_page_writable(sched_task_as(current), dst));
    assert(*(uint8_t *)mm_phys_to_virt(original) == 0x83);
    assert(!syscall_mm_munmap(dst, 4096));

    assert(syscall_mm_mmap(src, 4096, 3, flags, UINT64_MAX, 0) == (int64_t)src);
    assert(syscall_handle_task_page_fault(current, src, 6));
    original = address_space_user_page_phys(sched_task_as(current), src);
    *(uint8_t *)mm_phys_to_virt(original) = 0x4d;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    assert(!syscall_mm_mlock(src, 4096, LINUX_MLOCK_ONFAULT));
    assert(address_space_user_page_flags(sched_task_as(current), src) & RELIEFNT_PAGE_COW);
    const uint64_t failure_dst = 0x68000000;
    fail_page = true;
    assert(syscall_mm_mremap(src, 4096, 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, failure_dst) == -RELIEFOS_ENOMEM);
    fail_page = false;
    assert(address_space_user_page_phys(sched_task_as(current), src) == original);
    assert(!task_vma_containing(current, failure_dst, failure_dst + 4096));
    assert(syscall_mm_mremap(src, 4096, 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, failure_dst) == (int64_t)failure_dst);
    assert(address_space_user_page_phys(sched_task_as(current), failure_dst) == original);
    assert(address_space_user_page_flags(sched_task_as(current), failure_dst) & RELIEFNT_PAGE_COW);
    assert(syscall_handle_task_page_fault(current, failure_dst, 7));
    uint64_t copy = address_space_user_page_phys(sched_task_as(current), failure_dst);
    assert(copy && copy != original && *(uint8_t *)mm_phys_to_virt(copy) == 0x4d);
    *(uint8_t *)mm_phys_to_virt(copy) = 0x55;
    assert(*(uint8_t *)mm_phys_to_virt(original) == 0x4d);
    assert(address_space_user_page_phys(&child, src) == original);
    address_space_destroy(&child);
    assert(!syscall_mm_munmap(failure_dst, 4096));

    const uint64_t two_src = 0x7c000000, two_dst = 0x7e1ff000;
    assert(syscall_mm_mmap(two_src, 8192, 3, flags, UINT64_MAX, 0) == (int64_t)two_src);
    for (unsigned i = 0; i < 2; ++i)
        assert(syscall_handle_task_page_fault(current, two_src + i * 4096, 6));
    uint64_t left = address_space_user_page_phys(sched_task_as(current), two_src);
    uint64_t right = address_space_user_page_phys(sched_task_as(current), two_src + 4096);
    *(uint8_t *)mm_phys_to_virt(left) = 0xa1;
    *(uint8_t *)mm_phys_to_virt(right) = 0xb2;
    assert(!syscall_mm_mlock(two_src, 8192, LINUX_MLOCK_ONFAULT));
    unsigned retains_before = page_retains, flush_before = user_tlb_flush_calls;
    fail_page_after = 4;
    assert(syscall_mm_mremap(two_src, 8192, 8192,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, two_dst) == -RELIEFOS_ENOMEM);
    fail_page_after = -1;
    assert(page_retains >= retains_before + 2 && user_tlb_flush_calls > flush_before);
    assert(!task_vma_containing(current, two_dst, two_dst + 4096));
    assert(!address_space_user_page_phys(sched_task_as(current), two_dst));
    assert(address_space_user_page_phys(sched_task_as(current), two_src) == left);
    assert(address_space_user_page_phys(sched_task_as(current), two_src + 4096) == right);
    assert(*(uint8_t *)mm_phys_to_virt(left) == 0xa1 && *(uint8_t *)mm_phys_to_virt(right) == 0xb2);
    teardown();
}

/** @brief Reused physical roots receive fresh private MM identity generations. */
static void test_mm_identity_generation(void)
{
    struct address_space root = {.cr3 = 0xdead000};
    struct mm_identity first, second;
    struct mm_lock_intent a = {0}, b = {0}, replacement = {0};
    assert(!mm_context_identity(&root, &first));
    assert(!mm_context_lock_intent_begin(&root, 0x1000, 0x2000, &a));
    assert(!mm_context_lock_intent_begin(&root, 0x3000, 0x4000, &b));
    mm_context_lock_intent_end(&a);
    assert(!mm_context_lock_intent_intersects(&root, 0x1000, 0x2000));
    assert(mm_context_lock_intent_intersects(&root, 0x3000, 0x4000));
    mm_context_set_lock_flags(&root, LINUX_MCL_FUTURE);
    mm_context_destroy(&root);
    assert(!mm_lock_flags(&root));
    assert(!mm_context_identity(&root, &second));
    assert(first.cr3 == second.cr3 && first.generation != second.generation);
    assert(!mm_context_lock_intent_begin(&root, 0x3000, 0x4000, &replacement));
    mm_context_lock_intent_end(&b);
    assert(mm_context_lock_intent_intersects(&root, 0x3000, 0x4000));
    mm_context_lock_intent_end(&replacement);
    assert(!mm_context_lock_intent_intersects(&root, 0x3000, 0x4000));
    mm_context_destroy(&root);
}

/** @brief An OFD pin failure must not publish a partially owned lock split. */
static void test_mlock_split_pin_failure(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE;
    vma->mapping_file = drm_test_file;
    sched_task_limits(current)->memlock.rlim_cur = 4096;
    fail_file_get = true;
    assert(syscall_mm_mlock(base + 4096, 4096, LINUX_MLOCK_ONFAULT) == -RELIEFOS_ENOMEM);
    fail_file_get = false;
    vma = task_vma_containing(current, base, base + 5 * 4096);
    assert(vma && vma->mapping_file == drm_test_file && !(vma->flags & TASK_VMA_FLAG_LOCKED));
    teardown();
}

/** @brief A partial shared unmap retires pages no surviving span can refault. */
static void test_shared_partial_unmap_releases_page(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t address = 0x54000000;
    assert(syscall_mm_mmap(address, 3 * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    for (unsigned i = 0; i < 3; ++i)
        assert(syscall_handle_task_page_fault(current, address + i * 4096, 6));
    unsigned before = pages;
    assert(!syscall_mm_munmap(address + 4096, 4096));
    assert(pages == before - 1);
    assert(address_space_user_page_phys(sched_task_as(current), address));
    assert(address_space_user_page_phys(sched_task_as(current), address + 2 * 4096));
    teardown();
}

/** @brief Shared page lookup and range retirement have bounded index work. */
static void test_shared_index_scaling(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x6c000000;
    const unsigned count = 512;
    assert(syscall_mm_mmap(address, count * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    mm_shared_test_steps = 0;
    for (unsigned i = 0; i < count; ++i)
        assert(syscall_handle_task_page_fault(current, address + i * 4096, 6));
    assert(mm_shared_test_steps < 32 * count);
    uint64_t faults = mm_shared_test_steps;
    assert(!syscall_mm_madvise(address, count * 4096, LINUX_MADV_DONTNEED));
    mm_shared_test_steps = 0;
    assert(!syscall_mm_mincore(address, count * 4096, (uintptr_t)mincore_bytes));
    for (unsigned i = 0; i < count; ++i) assert(mincore_bytes[i] == 1);
    assert(mm_shared_test_steps < 32 * count);
    uint64_t residency = mm_shared_test_steps;
    unsigned before = pages;
    mm_shared_test_steps = 0;
    assert(!syscall_mm_munmap(address + 128 * 4096, 256 * 4096));
    assert(pages == before - 256);
    assert(mm_shared_test_steps < 32 * count);
    uint64_t prune = mm_shared_test_steps;
    teardown();
    printf("PASS shared index %u pages: fault=%llu residency=%llu partial-unmap=%llu steps\n",
           count, (unsigned long long)faults, (unsigned long long)residency,
           (unsigned long long)prune);
}

/** @brief Many distinct spans stay indexed across clone and alternating unmap. */
static void test_shared_span_index_scaling(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x70000000;
    const unsigned count = 128;
    uint64_t frames[128];
    for (unsigned i = 0; i < count; ++i)
        assert(syscall_mm_mmap(address + i * 8192, 4096, 3,
               LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
               UINT64_MAX, 0) == (int64_t)(address + i * 8192));
    mm_shared_test_steps = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t page = address + i * 8192;
        assert(syscall_handle_task_page_fault(current, page, 6));
        frames[i] = address_space_user_page_phys(sched_task_as(current), page);
        *(uint8_t *)mm_phys_to_virt(frames[i]) = (uint8_t)i;
    }
    uint64_t faults = mm_shared_test_steps;
    assert(faults < 48 * count);
    for (unsigned i = 0; i < count; ++i)
        assert(!syscall_mm_madvise(address + i * 8192, 4096, LINUX_MADV_DONTNEED));
    mm_shared_test_steps = 0;
    for (unsigned i = 0; i < count; ++i) {
        assert(!syscall_mm_mincore(address + i * 8192, 4096, (uintptr_t)mincore_bytes));
        assert(mincore_bytes[0] == 1);
    }
    uint64_t residency = mm_shared_test_steps;
    assert(residency < 32 * count);
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    mm_shared_test_steps = 0;
    for (unsigned i = 0; i < count; i += 2)
        assert(!syscall_mm_munmap(address + i * 8192, 4096));
    uint64_t prune = mm_shared_test_steps;
    assert(prune < 128 * count);
    for (unsigned i = 0; i < count; i += 2) {
        uint64_t page = address + i * 8192;
        assert(mm_shared_resident(&child, page));
        assert(!mm_shared_fault(&child, page, RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
        assert(address_space_user_page_phys(&child, page) == frames[i]);
        assert(*(uint8_t *)mm_phys_to_virt(frames[i]) == (uint8_t)i);
    }
    address_space_destroy(&child);
    teardown();
    printf("PASS shared index %u spans: fault=%llu residency=%llu half-unmap=%llu steps\n",
           count, (unsigned long long)faults, (unsigned long long)residency,
           (unsigned long long)prune);
}

/** @brief Failed split, clone and remap index allocations leave backing intact. */
static void test_shared_index_rollback(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x74000000, destination = 0x78000000;
    assert(syscall_mm_mmap(address, 3 * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    uint64_t backing[3];
    for (unsigned i = 0; i < 3; ++i) {
        assert(syscall_handle_task_page_fault(current, address + i * 4096, 6));
        backing[i] = address_space_user_page_phys(sched_task_as(current), address + i * 4096);
        *(uint8_t *)mm_phys_to_virt(backing[i]) = 0x31 + i;
    }
    unsigned before = pages, live_before = allocations - deallocations;
    fail_after = 0;
    assert(syscall_mm_munmap(address + 4096, 4096) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(pages == before && allocations - deallocations == live_before);
    assert(task_vma_containing(current, address, address + 3 * 4096));
    for (unsigned i = 0; i < 3; ++i)
        assert(address_space_user_page_phys(sched_task_as(current), address + i * 4096) == backing[i]);

    struct address_space child = {0};
    fail_after = 1; /* Child root metadata succeeds, first span allocation fails. */
    assert(!address_space_clone_cow(sched_task_as(current), &child));
    fail_after = -1;
    assert(pages == before && allocations - deallocations == live_before);
    assert(address_space_user_page_phys(sched_task_as(current), address) == backing[0]);

    fail_after = 2; /* Destination object and span succeed; alias staging fails. */
    assert(syscall_mm_mremap(address, 3 * 4096, 3 * 4096,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, destination) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(pages == before && allocations - deallocations == live_before);
    assert(!task_vma_containing(current, destination, destination + 4096));
    for (unsigned i = 0; i < 3; ++i) {
        assert(address_space_user_page_phys(sched_task_as(current), address + i * 4096) == backing[i]);
        assert(*(uint8_t *)mm_phys_to_virt(backing[i]) == 0x31 + i);
    }
    teardown();
}

/** @brief Many fork roots retain the same indexed object after partial unmap. */
static void test_shared_many_fork_owners(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x7a000000;
    const unsigned count = 64;
    assert(syscall_mm_mmap(address, 4 * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    for (unsigned i = 0; i < 4; ++i)
        assert(syscall_handle_task_page_fault(current, address + i * 4096, 6));
    uint64_t backing = address_space_user_page_phys(sched_task_as(current), address + 4096);
    *(uint8_t *)mm_phys_to_virt(backing) = 0x96;
    assert(!syscall_mm_madvise(address + 4096, 4096, LINUX_MADV_DONTNEED));
    struct address_space children[64] = {0};
    for (unsigned i = 0; i < count; ++i)
        assert(address_space_clone_cow(sched_task_as(current), &children[i]));
    mm_shared_test_steps = 0;
    assert(!syscall_mm_munmap(address + 4096, 4096));
    for (unsigned i = 0; i < count; ++i)
        assert(mm_shared_resident(&children[i], address + 4096));
    uint64_t steps = mm_shared_test_steps;
    assert(steps < 64 * count);
    assert(!mm_shared_fault(&children[0], address + 4096,
                            RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_phys(&children[0], address + 4096) == backing);
    assert(*(uint8_t *)mm_phys_to_virt(backing) == 0x96);
    for (unsigned i = 1; i < count; ++i)
        address_space_destroy(&children[(i * 37) % count]);
    assert(mm_shared_resident(&children[0], address + 4096));
    address_space_destroy(&children[0]);
    teardown();
    printf("PASS shared index %u fork owners: removal and probes=%llu steps\n",
           count, (unsigned long long)steps);
}

/** @brief A covering fork span avoids rescanning a resident range on unmap. */
static void test_shared_covered_range_prune(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x7b000000;
    const unsigned count = 512;
    assert(syscall_mm_mmap(address, count * 4096, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    for (unsigned i = 0; i < count; ++i)
        assert(syscall_handle_task_page_fault(current, address + i * 4096, 6));
    assert(!syscall_mm_madvise(address, count * 4096, LINUX_MADV_DONTNEED));
    struct mm_context *ctx = mm_context_borrow(sched_task_as(current));
    struct mm_shared_span *span = mm_index_overlap(ctx->shared, address, address + 4096)->owner;
    struct mm_shared_object *object = span->object;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    unsigned before = pages;
    mm_shared_test_steps = 0;
    assert(!syscall_mm_munmap(address + 128 * 4096, 256 * 4096));
    uint64_t steps = mm_shared_test_steps;
    assert(steps < 128 && pages == before);
    assert(mm_shared_resident(&child, address + 128 * 4096));
    address_space_destroy(&child);
    assert(!mm_page_lookup(object, 128));
    assert(mm_page_lookup(object, 127) && mm_page_lookup(object, 384));
    teardown();
    printf("PASS shared covered 256-page removal: %llu index steps\n",
           (unsigned long long)steps);
}

/** @brief Highest legal sparse object offsets do not truncate or alias page zero. */
static void test_shared_high_sparse_index(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    const uint64_t address = 0x100000000ULL;
    const uint64_t end = task_mmap_top(current);
    const uint64_t high = end - 4096;
    const uint64_t index = (high - address) / 4096;
    assert(index > (1ULL << 35) - (1ULL << 21));
    assert(syscall_mm_mmap(address, end - address, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    assert(syscall_handle_task_page_fault(current, address, 6));
    assert(syscall_handle_task_page_fault(current, high, 6));
    uint64_t first = address_space_user_page_phys(sched_task_as(current), address);
    uint64_t last = address_space_user_page_phys(sched_task_as(current), high);
    assert(first && last && first != last);
    *(uint8_t *)mm_phys_to_virt(first) = 0x2d;
    *(uint8_t *)mm_phys_to_virt(last) = 0xc7;
    assert(!syscall_mm_madvise(high, 4096, LINUX_MADV_DONTNEED));
    assert(mm_shared_resident(sched_task_as(current), high));
    assert(syscall_handle_task_page_fault(current, high, 6));
    assert(address_space_user_page_phys(sched_task_as(current), high) == last);
    assert(*(uint8_t *)mm_phys_to_virt(first) == 0x2d);
    assert(*(uint8_t *)mm_phys_to_virt(last) == 0xc7);
    struct mm_shared_object out_of_range = {0};
    uint64_t extra = mm_alloc_page();
    assert(!mm_page_insert(&out_of_range, 1ULL << 36, extra));
    assert(!out_of_range.pages && !mm_page_lookup(&out_of_range, 1ULL << 36));
    mm_free_page(extra);
    assert(!syscall_mm_munmap(address, end - address));
    teardown();
    printf("PASS shared sparse high index=%llu; 2^36 boundary rejected\n",
           (unsigned long long)index);
}

/** @brief A shared mapping stays one backing across fork before and after discard. */
static void test_shared_fork_without_resident_pte(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    uint64_t address = 0x56000000;
    assert(syscall_mm_mmap(address, 8192, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    assert(syscall_handle_task_page_fault(current, address, 6));
    uint64_t existing = address_space_user_page_phys(sched_task_as(current), address);
    *(uint8_t *)mm_phys_to_virt(existing) = 0x68;
    assert(!syscall_mm_madvise(address, 4096, LINUX_MADV_DONTNEED));
    struct task *child = calloc(1, sizeof(*child));
    assert(child);
    child->kind = TASK_KIND_USER;
    assert(address_space_clone_cow(sched_task_as(current), sched_task_as(child)));
    child->vmas[0] = *task_vma_containing(current, address, address + 8192);
    assert(!address_space_user_page_phys(sched_task_as(child), address));
    assert(syscall_handle_task_page_fault(current, address + 4096, 6));
    uint64_t after_fork = address_space_user_page_phys(sched_task_as(current), address + 4096);
    *(uint8_t *)mm_phys_to_virt(after_fork) = 0x39;
    assert(syscall_handle_task_page_fault(child, address + 4096, 6));
    assert(address_space_user_page_phys(sched_task_as(child), address + 4096) == after_fork);
    assert(!syscall_mm_munmap(address, 8192));
    assert(syscall_handle_task_page_fault(child, address, 6));
    assert(address_space_user_page_phys(sched_task_as(child), address) == existing);
    assert(*(uint8_t *)mm_phys_to_virt(existing) == 0x68);
    address_space_destroy(sched_task_as(child));
    free(child);
    teardown();
}

/** @brief Lock validation follows Linux capability and low-32-bit flag ordering. */
static void test_mlock_permission_and_flags(void)
{
    setup(1);
    sched_task_limits(current)->memlock.rlim_cur = 0;
    assert(syscall_mm_mlock(base, 0, 2) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mlock(base, 0, 0) == -RELIEFOS_EPERM);
    assert(syscall_mm_mlockall(LINUX_MCL_ONFAULT) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mlockall(LINUX_MCL_FUTURE) == -RELIEFOS_EPERM);
    assert(!syscall_mm_munlock(base, 0));
    current->cap_effective = 1ULL << CAP_IPC_LOCK;
    assert(!syscall_mm_mlock(base, 0, 1ULL << 32));
    assert(!syscall_mm_mlockall((1ULL << 32) | LINUX_MCL_FUTURE));
    assert(!syscall_mm_munlockall());
    teardown();
}

/** @brief FUTURE charges mappings and makes population a best-effort operation. */
static void test_mlockall_future_mapping(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    sched_task_limits(current)->memlock.rlim_cur = 4096;
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE));
    uint64_t address = 0x58000000;
    uint64_t map_flags = LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    assert(syscall_mm_mmap(address, 4096, 3, map_flags, UINT64_MAX, 0) == (int64_t)address);
    assert(address_space_user_page_phys(sched_task_as(current), address));
    assert(mm_locked_bytes(current) == 4096);
    assert(syscall_mm_mmap(address + 8192, 4096, 3, map_flags,
                           UINT64_MAX, 0) == -RELIEFOS_EAGAIN);
    assert(syscall_mm_mmap_dispatch(address + 8192, 4096, 3, map_flags,
                                    UINT64_MAX, 0) == -RELIEFOS_EAGAIN);
    assert(!syscall_mm_munmap(address, 4096));
    fail_page = true;
    assert(syscall_mm_mmap(address, 4096, 3, map_flags,
                           UINT64_MAX, 0) == (int64_t)address);
    fail_page = false;
    assert(task_vma_containing(current, address, address + 4096));
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(mm_locked_bytes(current) == 4096);
    assert(syscall_handle_task_page_fault(current, address, 6));
    assert(!syscall_mm_munmap(address, 4096));
    assert(syscall_mm_mmap(address, 4096, LINUX_PROT_NONE, map_flags,
                           UINT64_MAX, 0) == (int64_t)address);
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(mm_locked_bytes(current) == 4096);
    assert(!syscall_mm_mprotect(address, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE));
    assert(syscall_handle_task_page_fault(current, address, 6));
    assert(mm_locked_bytes(current) == 4096);
    assert(!syscall_mm_munmap(address, 4096));
    uint64_t shared_flags = LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS;
    fail_after = 0;
    assert(syscall_mm_mmap(address, 4096, 3, shared_flags, UINT64_MAX, 0) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(!task_vma_containing(current, address, address + 4096));
    fail_after = 2; /* Object and span admission succeed; radix population fails. */
    assert(syscall_mm_mmap(address, 4096, 3, shared_flags, UINT64_MAX, 0) == (int64_t)address);
    fail_after = -1;
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(!mm_shared_resident(sched_task_as(current), address));
    assert(mm_locked_bytes(current) == 4096);
    assert(syscall_handle_task_page_fault(current, address, 6));
    assert(!syscall_mm_munmap(address, 4096));
    sched_task_mm(current)->program_break_base = address;
    sched_task_mm(current)->program_break = address;
    fail_page = true;
    assert(syscall_mm_brk(address + 4096) == (int64_t)(address + 4096));
    fail_page = false;
    assert(!address_space_user_page_phys(sched_task_as(current), address));
    assert(mm_locked_bytes(current) == 4096);
    assert(syscall_handle_task_page_fault(current, address, 6));
    teardown();
}

/** @brief Lock across adjacent VMA slices while retaining the file offsets. */
static void test_mlock_across_vmas(void)
{
    setup(1);
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        *sched_task_vma_at(current, i) = (struct task_vma){0};
    *sched_task_vma_at(current, 0) = (struct task_vma){.used = 1, .start = base,
        .end = base + 3 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    *sched_task_vma_at(current, 1) = (struct task_vma){.used = 1,
        .start = base + 3 * 4096, .end = base + 6 * 4096, .prot = 3, .max_prot = 3,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_PRIVATE,
        .file_offset = 8192, .mapping_file = drm_test_file};
    sched_task_limits(current)->memlock.rlim_cur = 3 * 4096;
    assert(!syscall_mm_mlock(base + 4096, 3 * 4096, LINUX_MLOCK_ONFAULT));
    assert(mm_locked_bytes(current) == 3 * 4096);
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        assert(vma && !!(vma->flags & TASK_VMA_FLAG_LOCKED) == (i >= 1 && i < 4));
        if (i >= 3) {
            assert(vma->mapping_file == drm_test_file);
            assert(vma->file_offset + address - vma->start == 8192 + (i - 3) * 4096);
        }
    }
    assert(!syscall_mm_munlock(base + 2 * 4096, 2 * 4096));
    assert(mm_locked_bytes(current) == 4096);
    teardown();
}

/** @brief Moving a locked shared mapping does not charge its pages twice. */
static void test_locked_shared_mremap(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    sched_task_limits(current)->memlock.rlim_cur = 2 * 4096;
    uint64_t old = 0x5a000000, moved = 0x5c000000;
    assert(syscall_mm_mmap(old, 8192, 3,
           LINUX_MAP_SHARED | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)old);
    assert(!syscall_mm_mlock(old, 8192, LINUX_MLOCK_ONFAULT));
    assert(!syscall_mm_mlockall(LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT));
    assert(syscall_mm_mremap(old, 8192, 8192,
           LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED, moved) == (int64_t)moved);
    struct task_vma *vma = task_vma_containing(current, moved, moved + 8192);
    assert(vma && (vma->flags & TASK_VMA_FLAG_LOCKED));
    assert(vma->flags & TASK_VMA_FLAG_LOCKONFAULT);
    assert(mm_locked_bytes(current) == 8192);
    assert(syscall_mm_madvise(moved, 4096, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    teardown();
}

/** @brief Locked mremap growth keeps its policy when opportunistic prefault fails. */
static void test_locked_mremap_prefault_is_best_effort(void)
{
    setup(1);
    sched_task_limits(current)->as.rlim_cur = LINUX_RLIM_INFINITY;
    sched_task_limits(current)->memlock.rlim_cur = 2 * 4096;
    uint64_t address = 0x5e000000;
    assert(syscall_mm_mmap(address, 4096, 3,
           LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
           UINT64_MAX, 0) == (int64_t)address);
    assert(!syscall_mm_mlock(address, 4096, 0));
    fail_page = true;
    assert(syscall_mm_mremap(address, 4096, 8192, 0, 0) == (int64_t)address);
    fail_page = false;
    struct task_vma *growth = task_vma_containing(current, address + 4096, address + 8192);
    assert(growth && (growth->flags & TASK_VMA_FLAG_LOCKED));
    assert(!(growth->flags & TASK_VMA_FLAG_LOCKONFAULT));
    assert(!address_space_user_page_phys(sched_task_as(current), address + 4096));
    assert(syscall_handle_private_anon_fault(current, address + 4096, 6));
    teardown();
}

/** @brief Discard a private COW copy without changing a fork peer's content. */
static void test_dontneed_private_cow(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    vma->flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(syscall_handle_private_anon_fault(current, base, 6));
    uint64_t original = address_space_user_page_phys(sched_task_as(current), base);
    *(uint8_t *)mm_phys_to_virt(original) = 0x29;
    struct address_space child = {0};
    assert(address_space_clone_cow(sched_task_as(current), &child));
    assert(address_space_handle_cow_fault(sched_task_as(current), base));
    uint64_t copy = address_space_user_page_phys(sched_task_as(current), base);
    assert(copy != original);
    *(uint8_t *)mm_phys_to_virt(copy) = 0x72;
    assert(!syscall_mm_madvise(base, 4096, LINUX_MADV_DONTNEED));
    assert(syscall_handle_private_anon_fault(current, base, 6));
    assert(*(uint8_t *)mm_phys_to_virt(address_space_user_page_phys(sched_task_as(current), base)) == 0);
    assert(address_space_user_page_phys(&child, base) == original);
    assert(*(uint8_t *)mm_phys_to_virt(original) == 0x29);
    address_space_destroy(&child);
    teardown();
}

/** @brief A stack-gap discard stops before a later locked stack slice. */
static void test_dontneed_implicit_stack_locked_boundary(void)
{
    setup(1);
    uint64_t top = RELIEFNT_USER_TOP - 4096, low = top - 3 * 4096;
    current->stack_top = top;
    current->stack_low = low;
    sched_task_mm(current)->initial_stack_top = top;
    sched_task_mm(current)->initial_stack_low = low;
    sched_task_limits(current)->memlock.rlim_cur = 4096;
    for (uint64_t page = low; page < top; page += 4096)
        assert(address_space_map_user_stack_page(sched_task_as(current), page));
    uint64_t locked = address_space_user_page_phys(sched_task_as(current), low + 4096);
    assert(!syscall_mm_mlock(low + 4096, 4096, LINUX_MLOCK_ONFAULT));
    assert(syscall_mm_madvise(low, 3 * 4096, LINUX_MADV_DONTNEED) == -RELIEFOS_EINVAL);
    assert(!address_space_user_page_phys(sched_task_as(current), low));
    assert(address_space_user_page_phys(sched_task_as(current), low + 4096) == locked);
    assert(address_space_user_page_phys(sched_task_as(current), low + 8192));
    teardown();
}

static void test_split(unsigned extra, unsigned first, unsigned last)
{
    setup(extra);
    uint64_t start = base + first * 4096, end = base + last * 4096;
    /* Leave len unaligned: Linux/POSIX apply protection through the last page. */
    assert(syscall_mm_mprotect(start, end - start - 1,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        assert(vma);
        bool writable = i >= first && i < last;
        assert(vma->prot == (writable ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_NONE));
        assert(syscall_handle_private_anon_fault(current, address + 17, 6) == writable);
        assert(address_space_user_page_writable(sched_task_as(current), address) == writable);
    }
    uint64_t phys = address_space_user_page_phys(sched_task_as(current), start);
    assert(phys);
    *(unsigned char *)(uintptr_t)phys = 42;
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_NONE) == 0);
    assert(!address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_READ) == 0);
    assert(address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(*(unsigned char *)(uintptr_t)phys == 42);
    teardown();
}

static void test_allocation_failure(unsigned extra)
{
    setup(extra);
    uint32_t index = SCHED_TASK_VMA_MAX + extra - 1;
    struct task_vma saved = *sched_task_vma_at(current, index);
    fail_after = 0;
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(!memcmp(&saved, sched_task_vma_at(current, index), sizeof(saved)));
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        assert(sched_task_vma_at(current, i)->used != 0xffffffffu);
    assert(!syscall_handle_private_anon_fault(current, base + 4096, 6));
    assert(syscall_mm_mprotect(base + 1, 4096, LINUX_PROT_READ) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mprotect(base - 4096, 4096, LINUX_PROT_READ) == -RELIEFOS_ENOMEM);
    sched_task_vma_at(current, index)->max_prot = LINUX_PROT_READ;
    assert(syscall_mm_mprotect(base, 5 * 4096, LINUX_PROT_WRITE) == -RELIEFOS_EACCES);
    teardown();
}

static void test_append_with_spare_capacity(void)
{
    setup(1);
    struct task_vma *store = current->vma_extra;
    unsigned before = allocations;
    assert(sched_task_vma_at(current, SCHED_TASK_VMA_MAX + 1));
    assert(current->vma_extra == store && allocations == before);
    assert(current->vma_extra_count == 2);
    teardown();
}

static void test_adjacent_vmas(void)
{
    setup(4);
    unsigned count = SCHED_TASK_VMA_MAX + 4;
    struct task_vma *left = sched_task_vma_at(current, count - 1);
    struct task_vma *right = sched_task_vma_at(current, count - 2);
    assert(left && right);
    left->end = base + 3 * 4096;
    right->start = left->end;
    right->end = base + 5 * 4096;
    right->prot = LINUX_PROT_READ;
    /* Linux accepts one mprotect range spanning two adjacent VMAs. */
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        uint32_t expected = i == 0 ? LINUX_PROT_NONE :
                            i < 4 ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_READ;
        assert(vma && vma->prot == expected);
        bool writable = i > 0 && i < 4;
        assert(syscall_handle_private_anon_fault(current, address, 6) == writable);
    }
    teardown();
}

static void device_test_release(void *value){assert(value);}
static uint64_t device_test_physical(void *value,uint64_t offset){return *(uint64_t *)value+offset;}
static void test_drm_mprotect_flushes_remote_translations(void)
{
    setup(1);
    uint32_t last = sched_task_vma_capacity(current) - 1;
    struct task_vma *vma = sched_task_vma_at(current, last);
    assert(vma && vma->used);
    uint64_t physical = mm_alloc_page();
    vma->start = base;
    vma->end = base + 4096;
    vma->flags = TASK_VMA_FLAG_DEVICE | TASK_VMA_FLAG_SHARED |
                 TASK_VMA_FLAG_MMAP;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    vma->max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    static const struct device_mapping_operations operations={.release=device_test_release,.physical=device_test_physical};
    struct device_mapping_info info={.length=4096,.max_prot=3,.can_split=true,.can_move=true};
    assert(!device_mapping_create(NULL,&operations,&physical,&info,&vma->device_mapping));
    vma->device_physical = physical;
    assert(address_space_map_user_page(sched_task_as(current), base, physical,
           RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED |
           RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC |
           RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT));
    assert(address_space_user_page_writable(sched_task_as(current), base));

    user_tlb_flush_calls = 0;
    user_tlb_flush_probe_page = base;
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ) == 0);
    assert(user_tlb_flush_calls == 1);
    assert(!address_space_user_page_writable(sched_task_as(current), base));
    assert((address_space_user_page_flags(sched_task_as(current),base) &
           (RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT))==
           (RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT));
    assert(sched_task_vma_at(current, last)->prot == LINUX_PROT_READ);

    assert(syscall_mm_munmap(base, 4096) == 0);
    user_tlb_flush_probe_page = 0;
    mm_free_page(physical);
    teardown();
}

static void test_elf_source_split_and_clone(void)
{
    setup(0);
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.size=6*4096,.first_cluster=1};
    struct vfs_exec_source *source=exec_fixture_open(&node);
    struct task_vma *vma=sched_task_vma_at(current,SCHED_TASK_VMA_MAX-1);
    *vma=(struct task_vma){.used=1,.start=base,.end=base+6*4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.max_prot=LINUX_PROT_READ|LINUX_PROT_WRITE,
        .flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_PRIVATE,
        .file_node=node,.file_limit=node.size,.exec_source=source};
    assert(!syscall_mm_mprotect(base+4096,3*4096,LINUX_PROT_READ));
    vma=task_vma_containing(current,base+4096,base+2*4096);
    assert(vma && vma->exec_source==source);
    struct task_vma copy=*vma;copy.exec_source=NULL;
    assert(!sched_task_vma_retain_file(&copy,vma));
    assert(!syscall_mm_munmap(base,6*4096));
    assert(exec_fixture_pins==2 && exec_fixture_inode_refs==1 && exec_fixture_denials==1);
    task_vma_clear(&copy);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
    teardown();
    puts("PASS ELF source survives mprotect splits, cloned VMA and original unmap; last mapping releases provider and writer denial");
}

struct movable_device_test {uint64_t physical[3];unsigned releases;};
static void movable_device_release(void *value)
{
    struct movable_device_test *state=value;++state->releases;
    for(unsigned i=0;i<3;++i)mm_free_page(state->physical[i]);
}
static uint64_t movable_device_physical(void *value,uint64_t offset)
{return ((struct movable_device_test *)value)->physical[offset/4096];}

/** @brief Partial lock/unlock cannot split a provider or raw device VMA. */
static void test_device_lock_split_contract(void)
{
    for (unsigned retained = 0; retained < 2; ++retained) {
        setup(1);
        sched_task_limits(current)->memlock.rlim_cur = 3 * 4096;
        struct movable_device_test state = {0};
        for (unsigned i = 0; i < 3; ++i) state.physical[i] = mm_alloc_page();
        struct task_vma *vma = task_vma_containing(current, base, base + 3 * 4096);
        *vma = (struct task_vma){.used = 1, .start = base, .end = base + 3 * 4096,
            .prot = 3, .max_prot = 3,
            .flags = TASK_VMA_FLAG_DEVICE | TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_MMAP};
        static const struct device_mapping_operations operations = {
            .release = movable_device_release, .physical = movable_device_physical};
        struct device_mapping_info info = {.length = 3 * 4096, .max_prot = 3,
            .can_split = false, .can_move = false};
        if (retained)
            assert(!device_mapping_create(NULL, &operations, &state, &info, &vma->device_mapping));
        for (unsigned i = 0; i < 3; ++i)
            assert(address_space_map_user_page(sched_task_as(current), base + i * 4096,
                   state.physical[i], RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED |
                   RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
        struct device_mapping *owner = vma->device_mapping;
        assert(syscall_mm_mlock(base + 4096, 4096, LINUX_MLOCK_ONFAULT) == -RELIEFOS_EOPNOTSUPP);
        assert(!mm_locked_bytes(current) && vma->device_mapping == owner && !state.releases);
        assert(syscall_mm_munmap(base + 4096, 4096) == -RELIEFOS_EOPNOTSUPP);
        assert(!syscall_mm_mlock(base, 3 * 4096, LINUX_MLOCK_ONFAULT));
        assert(syscall_mm_munlock(base + 4096, 4096) == -RELIEFOS_EOPNOTSUPP);
        assert(mm_locked_bytes(current) == 3 * 4096 && vma->device_mapping == owner && !state.releases);
        assert(syscall_mm_munmap(base + 4096, 4096) == -RELIEFOS_EOPNOTSUPP);
        assert(!syscall_mm_munmap(base, 3 * 4096));
        if (!retained)
            for (unsigned i = 0; i < 3; ++i) mm_free_page(state.physical[i]);
        else assert(state.releases == 1);
        teardown();
    }
}

static void test_device_remap_preserves_actual_pages_and_shared_lease(void)
{
    setup(1);current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct movable_device_test state={0};
    for(unsigned i=0;i<3;++i)state.physical[i]=mm_alloc_page();
    static const struct device_mapping_operations operations={
        .release=movable_device_release,.physical=movable_device_physical};
    struct device_mapping_info info={.length=3*4096,.max_prot=3,.can_split=true,.can_move=true};
    struct task_vma *vma=sched_task_vma_at(current,sched_task_vma_capacity(current)-1);
    *vma=(struct task_vma){.used=1,.start=base,.end=base+3*4096,.prot=3,.max_prot=3,
        .flags=TASK_VMA_FLAG_DEVICE|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_MMAP,
        .device_physical=state.physical[0]};
    assert(!device_mapping_create(NULL,&operations,&state,&info,&vma->device_mapping));
    struct task_vma clone=*vma;clone.device_mapping=NULL;
    assert(!sched_task_vma_retain_device(&clone,vma));
    const uint64_t policy[]={RELIEFNT_PAGE_PWT,RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT,
                             RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD};
    const uint64_t mask=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT;
    for(unsigned i=0;i<3;++i)assert(address_space_map_user_page(sched_task_as(current),base+i*4096,
        state.physical[i],RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC|policy[i]));
    uint64_t target=base+0x1000000;
    assert(syscall_mm_mremap(base,3*4096,4*4096,LINUX_MREMAP_MAYMOVE,0)==-RELIEFOS_EFAULT);
    assert(syscall_mm_mremap(base,3*4096,3*4096,LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED,target+1)==-RELIEFOS_EINVAL);
    assert(address_space_user_page_phys(sched_task_as(current),base)==state.physical[0]);
    assert(syscall_mm_mremap(base,3*4096,2*4096,LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED,target)==(int64_t)target);
    assert(!address_space_user_page_phys(sched_task_as(current),base));
    for(unsigned i=0;i<2;++i)assert(address_space_user_page_phys(sched_task_as(current),target+i*4096)==state.physical[i]);
    for(unsigned i=0;i<2;++i)assert((address_space_user_page_flags(sched_task_as(current),target+i*4096)&mask)==policy[i]);
    assert(!syscall_mm_mprotect(target+4096,4096,LINUX_PROT_READ));
    assert((address_space_user_page_flags(sched_task_as(current),target+4096)&mask)==policy[1]);
    assert(!syscall_mm_munmap(target,4096));
    vma=task_vma_containing(current,target+4096,target+2*4096);
    assert(vma && vma->device_map_offset==4096 && vma->device_physical==state.physical[1]);
    assert(!syscall_mm_munmap(target+4096,4096));assert(!state.releases);
    task_vma_clear(&clone);assert(state.releases==1);
    teardown();
    puts("PASS generic device mremap resolves each actual PFN, preserves source on invalid move, forbids expansion and shares final backing through cloned/split VMAs");
}

/* Catch a syscall/PTE rejection of HotSpot's W -> W|X transition, and an
 * eager/lazy anonymous mapper silently restoring NX after explicit EXEC. */
static void test_explicit_executable_anonymous_memory(void)
{
    setup(1);
    struct task_vma *vma = task_vma_containing(current, base, base + 4096);
    assert(vma);
    vma->prot = LINUX_PROT_WRITE;
    assert(syscall_handle_private_anon_fault(current, base, 6));
    uint64_t physical = address_space_user_page_phys(sched_task_as(current), base);
    *(unsigned char *)mm_phys_to_virt(physical) = 42;
    assert(address_space_user_page_flags(sched_task_as(current), base) & RELIEFNT_PAGE_NOEXEC);
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_WRITE | LINUX_PROT_EXEC) == 0);
    assert(address_space_user_page_writable(sched_task_as(current), base));
    assert(!(address_space_user_page_flags(sched_task_as(current), base) & RELIEFNT_PAGE_NOEXEC));
    assert(*(unsigned char *)mm_phys_to_virt(physical) == 42);

    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ | LINUX_PROT_EXEC) == 0);
    assert(!address_space_user_page_writable(sched_task_as(current), base));
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC) == 0);
    assert(address_space_handle_cow_fault(sched_task_as(current), base));
    assert(address_space_user_page_writable(sched_task_as(current), base));
    assert(!(address_space_user_page_flags(sched_task_as(current), base) & RELIEFNT_PAGE_NOEXEC));

    assert(syscall_mm_mprotect(base + 4096, 4096, LINUX_PROT_WRITE | LINUX_PROT_EXEC) == 0);
    assert(syscall_handle_private_anon_fault(current, base + 4096, 20));
    assert(address_space_user_page_writable(sched_task_as(current), base + 4096));
    assert(!(address_space_user_page_flags(sched_task_as(current), base + 4096) & RELIEFNT_PAGE_NOEXEC));
    assert(task_map_anonymous_pages(current, base + 8192, base + 12288,
                                    RELIEFNT_PAGE_WRITABLE) == 0);
    assert(!(address_space_user_page_flags(sched_task_as(current), base + 8192) & RELIEFNT_PAGE_NOEXEC));

    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    assert(address_space_user_page_flags(sched_task_as(current), base) & RELIEFNT_PAGE_NOEXEC);
    uint64_t saved = address_space_user_page_flags(sched_task_as(current), base);
    vma = task_vma_containing(current, base, base + 4096);
    vma->max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC) == -RELIEFOS_EACCES);
    assert(address_space_user_page_flags(sched_task_as(current), base) == saved);
    assert(syscall_mm_mprotect(base, 4096, 0x80) == -RELIEFOS_EINVAL);
    teardown();
}

/** @brief Exercise ordinary and reclaim context allocation and page ownership. */
static void test_reclaim_context_emergency_page(void)
{
    struct address_space as = {.cr3 = 0x12345000};
    unsigned initial_pages = pages, initial_attempts = emergency_page_attempts;
    uint64_t generation = 0;
    fail_after = 0;
    fail_page = true;
    assert(mm_context_ensure(&as) == -RELIEFOS_ENOMEM);
    assert(emergency_page_attempts == initial_attempts);
    assert(mm_context_ensure_reclaim_generation(&as, &generation) == -RELIEFOS_ENOMEM);
    assert(emergency_page_attempts == initial_attempts + 1 && pages == initial_pages);
    fail_page = false;
    assert(!mm_context_ensure_reclaim_generation(&as, &generation));
    struct mm_context *ctx = mm_context_borrow(&as);
    assert(generation && ctx && ctx->emergency_physical && pages == initial_pages + 1);
    assert(emergency_page_attempts == initial_attempts + 2);
    fail_after = -1;
    mm_context_destroy(&as);
    assert(pages == initial_pages && !mm_context_borrow(&as));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "fix1-device-cache")) {
        test_drm_mprotect_flushes_remote_translations();
        test_device_remap_preserves_actual_pages_and_shared_lease();return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix5-current-peer")) {
        test_checked_eager_current_same_mm_peer(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix5-stack-modes")) {
        test_current_implicit_stack_modes(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix4-noop-peer")) {
        test_checked_noop_same_mm_peer(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix4-effective")) {
        test_effective_revision_boundaries(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix4-sysv-failure")) {
        test_failed_sysv_reservation_revision(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-guards")) {
        test_population_many_guard_search_budget(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-eio")) {
        test_population_local_eio_then_healthy(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-split")) {
        test_population_unvisited_split_reentry(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-early")) {
        test_checked_early_failure_preserves_locks(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-late")) {
        test_checked_late_failure_preserves_locks(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-atomic")) {
        test_checked_split_failure_is_atomic(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-atomic-peers")) {
        test_unlock_and_current_split_failure_is_atomic(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-intent")) {
        test_checked_intent_reentry(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "fix3-provider")) {
        test_checked_provider_failure_preserves_locks(); return 0;
    }
    test_reclaim_context_emergency_page();
    test_population_many_guard_search_budget();
    test_population_local_eio_then_healthy();
    test_population_unvisited_split_reentry();
    test_checked_long_scan_nonmutating_peer();
    test_vma_revision_mutation_coverage();
    test_checked_noop_same_mm_peer();
    test_checked_eager_current_same_mm_peer();
    test_current_implicit_stack_modes();
    test_effective_revision_boundaries();
    test_failed_sysv_reservation_revision();
    test_checked_early_failure_preserves_locks();
    test_checked_late_failure_preserves_locks();
    test_checked_split_failure_is_atomic();
    test_unlock_and_current_split_failure_is_atomic();
    test_checked_intent_reentry();
    test_checked_provider_failure_preserves_locks();
    test_population_guard_operation_count();
    test_best_effort_population_failure_bounds();
    test_population_cooperative_reentry();
    test_private_locked_fixed_mremap();
    test_mlockall_current_guard_and_oom();
    test_device_lock_split_contract();
    test_shared_index_scaling();
    test_shared_span_index_scaling();
    test_shared_index_rollback();
    test_shared_many_fork_owners();
    test_shared_covered_range_prune();
    test_shared_high_sparse_index();
    test_dontneed_private_retirement();
    test_mlock_partial_onfault_and_quota();
    test_mlock_implicit_initial_stack();
    test_mlockall_shared_mm_policy();
    test_mlock_prefault();
    test_mlockall_current_stack_and_quota();
    test_dontneed_shared_anonymous();
    test_shared_fault_ownership_failure();
    test_mincore_residency_and_bounds();
    test_dontneed_holes_and_rejections();
    test_mlockall_future_stack_growth();
    test_shared_anonymous_mremap();
    test_mm_identity_generation();
    test_mlock_split_pin_failure();
    test_shared_partial_unmap_releases_page();
    test_shared_fork_without_resident_pte();
    test_mlock_permission_and_flags();
    test_mlockall_future_mapping();
    test_mlock_across_vmas();
    test_locked_shared_mremap();
    test_locked_mremap_prefault_is_best_effort();
    test_dontneed_private_cow();
    test_dontneed_implicit_stack_locked_boundary();
    test_explicit_executable_anonymous_memory();
    for (unsigned extra = 1; extra <= 17; ++extra) {
        test_split(extra, 1, 5); /* musl stack guard: prefix split */
        test_split(extra, 0, 4); /* suffix split */
        test_split(extra, 1, 4); /* both sides, including a growth between slots */
    }
    test_allocation_failure(15); /* one spare append before the allocation fails */
    test_allocation_failure(16); /* allocation fails on the first needed slot */
    test_append_with_spare_capacity();
    test_adjacent_vmas();
    test_drm_mprotect_flushes_remote_translations();
    test_device_remap_preserves_actual_pages_and_shared_lease();
    test_elf_source_split_and_clone();
    puts("PASS mprotect: VMA growth, guard pages, lazy writes, page rounding, revoke/restore and errno");
    assert(allocations);
    return 0;
}

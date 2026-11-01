/* Pressure orchestration linked to production code; dependencies are explicit
 * finite CPU/cache/storage models. Real PTE/slot paths have separate fixtures. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/mm.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <linux/errno.h>

enum { TEST_ATOMIC = 1, TEST_NOIO = 2, TEST_NO_SELF_MM = 4 };
extern int memory_try_reclaim(uint32_t, uint32_t) __attribute__((weak));
extern void memory_pressure_poll(uint32_t) __attribute__((weak));
extern void memory_reclaim_fault_scope_begin(struct task *, uint64_t, uint64_t) __attribute__((weak));
extern void memory_reclaim_fault_scope_end(void) __attribute__((weak));
extern int memory_try_reclaim_no_oom(uint64_t) __attribute__((weak));
static uint64_t free_pages = 64;
static bool jitter_free;
static unsigned free_reads;
static bool writer = true, cache_clean, cache_full_budget, storage_same, storage_other, storage_slot;
static bool same_mm_fault, no_candidates, allow_current_candidate;
static unsigned expected_same_index = 5;
static unsigned cache_calls, swap_calls, oom_calls, max_budget;
static uint64_t cache_epoch;
static struct pressure_cursor fault_cursors[2];
static struct task current = {.pid=3,.kind=TASK_KIND_USER,.state=TASK_RUNNING},
                   other = {.pid=4,.kind=TASK_KIND_USER,.state=TASK_READY};

uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
uint64_t mm_free_memory_kib(void)
{ return (free_pages + (jitter_free && (free_reads++ & 1u) ? 1u : 0u)) * 4; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
bool kernel_execution_is_outermost(void) { return writer; }
struct task *sched_current_task(void) { return &current; }
uint32_t sched_current_pid(void) { return current.pid; }
struct task *sched_task_at(uint32_t i) { return i == 0 ? &current : i == 1 ? &other : NULL; }
uint64_t mm_context_generation_existing(const struct address_space *as)
{ return as && as->cr3 ? 1 : 0; }
int mm_context_ensure_reclaim_generation(struct address_space *as, uint64_t *generation)
{ if (!as || !as->cr3) return -ENOMEM; *generation = 1; return 0; }
bool swap_storage_current_owner(void) { return storage_same; }
bool swap_storage_other_owner(void) { return storage_other; }
bool swap_storage_slot_active(void) { return storage_slot; }
bool swap_storage_busy(void) { return storage_same || storage_other || storage_slot; }
uint32_t page_cache_pressure_scan(uint32_t budget, uint32_t *inspected, bool *cycle)
{
    ++cache_calls; if (budget > max_budget) max_budget = budget;
    *inspected = cache_full_budget ? budget : 1; *cycle = true;
    ++cache_epoch;
    if (cache_clean) { cache_clean = false; ++free_pages; return 1; }
    return 0;
}
uint32_t page_cache_pressure_scan_count(uint32_t budget, uint32_t *inspected, bool *cycle, uint32_t *candidates)
{ uint32_t freed=page_cache_pressure_scan(budget,inspected,cycle); *candidates=freed; return freed; }
uint64_t page_cache_pressure_epoch(void) { return cache_epoch; }
struct pressure_cursor *sched_pressure_cursor(struct task *task, bool allow_oom)
{ assert(task == &current); return &fault_cursors[allow_oom ? 1 : 0]; }
int paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page)
{
    if (no_candidates) return 0;
    if (same_mm_fault && as == sched_task_as(&current)) {
        unsigned index = (unsigned)((*cursor - RELIEFNT_USER_BASE) / 4096);
        if (index >= 7) return 0;
        *page = *cursor; *cursor += 4096; return 2;
    }
    if (*cursor != RELIEFNT_USER_BASE) return 0;
    *page = *cursor; *cursor += 4096;
    assert(as->cr3); return 2;
}
int swap_try_evict(struct address_space *as, uint64_t page)
{
    if (same_mm_fault) {
        printf("same-MM selected page=%llu\n", (unsigned long long)((page-RELIEFNT_USER_BASE)/4096));
        fflush(stdout);
        assert(as == sched_task_as(&current) && page == RELIEFNT_USER_BASE + expected_same_index * 4096);
        ++swap_calls; ++free_pages; return 1;
    }
    assert(as == sched_task_as(allow_current_candidate ? &current : &other) &&
           page == RELIEFNT_USER_BASE);
    assert(!storage_same && !storage_other && !storage_slot);
    ++swap_calls; ++free_pages; return 1;
}
int memory_oom_advance(uint32_t budget)
{ assert(budget <= 32); ++oom_calls; return -ENOMEM; }
/* This orchestration fixture has no queue waiters. */
void memory_pressure_notify(void) { }
void sched_pressure_fault_pages(const struct task *task, uint64_t rip, uint64_t pages[4])
{
    assert(task == &current && rip == RELIEFNT_USER_BASE + 16);
    pages[0] = RELIEFNT_USER_BASE + 4096; pages[1] = RELIEFNT_USER_BASE + 2 * 4096;
    pages[2] = RELIEFNT_USER_BASE + 3 * 4096; pages[3] = 0;
}

int main(int argc, char **argv)
{
    sched_task_as(&current)->cr3 = 0x1000;
    sched_task_as(&other)->cr3 = 0x2000;
    if (argc == 2 && !strcmp(argv[1], "jitter")) {
        no_candidates = true; free_pages = 69; jitter_free = true; free_reads = 1;
        int rc; unsigned batches = 0;
        do {
            rc = memory_try_reclaim_no_oom ? memory_try_reclaim_no_oom(6) : memory_try_reclaim(6, 0);
            assert(++batches < 10);
        } while (rc == -EAGAIN);
        if (!rc) fprintf(stderr, "free-count jitter falsely reported a released frame\n");
        assert(rc == -ENOMEM && !oom_calls && !swap_calls);
        puts("PASS unrelated free-count jitter does not report reclaim progress");
        return 0;
    }
    assert(argc == 1);
    cache_clean = true;
    int rc = memory_try_reclaim ? memory_try_reclaim(1, TEST_NO_SELF_MM) : -ENOMEM;
    printf("low-free cache reclaim status=%d cache=%u swap=%u\n", rc, cache_calls, swap_calls);
    fflush(stdout);
    assert(rc == 0 && cache_calls == 1 && !swap_calls && free_pages == 65);
    free_pages = 64;
    rc = memory_try_reclaim(1, TEST_NO_SELF_MM);
    assert(rc == 0 && swap_calls == 1 && free_pages == 65);
    free_pages = 64; unsigned before = swap_calls;
    assert(memory_try_reclaim(1, TEST_ATOMIC) == -EAGAIN && swap_calls == before);
    writer = false;
    assert(memory_try_reclaim(1, 0) == -EAGAIN && swap_calls == before);
    writer = true;
    assert(memory_try_reclaim(1, TEST_NOIO) < 0 && swap_calls == before);
    storage_same = true;
    assert(memory_try_reclaim(1, TEST_NO_SELF_MM) == -EAGAIN && swap_calls == before);
    storage_same = false; storage_other = true;
    assert(memory_try_reclaim(1, TEST_NO_SELF_MM) == -EAGAIN && swap_calls == before);
    storage_other = false; storage_slot = true;
    assert(memory_try_reclaim(1, TEST_NO_SELF_MM) == -EAGAIN && swap_calls == before);
    storage_slot = false;
    cache_clean = true; cache_full_budget = true; free_pages = 64 + 127;
    before = swap_calls;
    memory_pressure_poll(100);
    assert(max_budget <= 32 && free_pages == 64 + 128 && swap_calls == before);
    assert(!oom_calls);
    same_mm_fault = true; cache_full_budget = false; free_pages = 64;
    if (memory_reclaim_fault_scope_begin)
        memory_reclaim_fault_scope_begin(&current, RELIEFNT_USER_BASE + 16,
                                         RELIEFNT_USER_BASE + 4 * 4096);
    assert(memory_try_reclaim(1, 0) == 0 && free_pages == 65);
    if (memory_reclaim_fault_scope_end) memory_reclaim_fault_scope_end();
    unsigned cache_before = cache_calls;
    free_pages = 64; expected_same_index = 6;
    if (memory_reclaim_fault_scope_begin)
        memory_reclaim_fault_scope_begin(&current, RELIEFNT_USER_BASE + 16,
                                         RELIEFNT_USER_BASE + 5 * 4096);
    assert(memory_try_reclaim(1, 0) == 0 && free_pages == 65);
    if (memory_reclaim_fault_scope_end) memory_reclaim_fault_scope_end();
    assert(cache_calls == cache_before);
    /* With several free pages, an allocation failure may need more backing
     * frames; this bounded scan may swap but must not choose an OOM victim. */
    same_mm_fault = false; no_candidates = false; allow_current_candidate = true;
    free_pages = 69; before = swap_calls;
    rc = memory_try_reclaim_no_oom ? memory_try_reclaim_no_oom(6) : memory_try_reclaim(6, 0);
    while (rc == -EAGAIN)
        rc = memory_try_reclaim_no_oom ? memory_try_reclaim_no_oom(6) : memory_try_reclaim(6, 0);
    assert(!rc && free_pages == 70 && swap_calls == before + 1 && !oom_calls);
    no_candidates = true; free_pages = 69;
    unsigned batches = 0;
    do {
        rc = memory_try_reclaim_no_oom ? memory_try_reclaim_no_oom(6) : memory_try_reclaim(6, 0);
        assert(++batches < 10);
    } while (rc == -EAGAIN);
    if (oom_calls) fprintf(stderr, "no-progress high-free reclaim entered OOM selection\n");
    assert(rc == -ENOMEM && !oom_calls);
    /* More than 2^32 ordinary pages must not truncate the requested target. */
    free_pages = (1ULL << 32) + 64; batches = 0;
    do {
        rc = memory_try_reclaim_no_oom ? memory_try_reclaim_no_oom((1ULL << 32) + 1) :
                                         memory_try_reclaim((uint32_t)((1ULL << 32) + 1), 0);
        assert(++batches < 10);
    } while (rc == -EAGAIN);
    assert(rc == -ENOMEM && !oom_calls);
    puts("PASS low watermark, cache before swap, NOIO/owner/atomic gates and budget");
    return 0;
}

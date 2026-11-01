/* SPDX-License-Identifier: Apache-2.0 */
/* Real trap dispatcher, execution gate and paging code; disk transport alone
 * is replaced. Including the dispatcher exposes its private fault spinlock to
 * this boundary fixture without adding test instrumentation to production. */
#define main private_fault_fixture_main
#include "private_fault_dispatch_test.c"
#undef main
#include "../../arch/x86_64/interrupt/idt.c"
#include "../../mm/swap/internal.h"
#ifndef SWAP_STORAGE_CONTENDED
#define SWAP_STORAGE_CONTENDED (-4100)
#endif

#ifndef GATE_MEDIA_PAGES
#define GATE_MEDIA_PAGES 4
#endif
static uint8_t gate_media[GATE_MEDIA_PAGES][4096];
static unsigned gate_reads, gate_writes;
static bool gate_contended;
static unsigned gate_sleeps;

#ifndef PRESSURE_REAL_FIXTURE
/* The storage-contention control models one timed wait; queue ownership and
 * the finite retry limit are exercised by the real pressure-wait fixture. */
int memory_fault_storage_wait(struct task *task, uint64_t address)
{
    assert(task == current && address == fault_address);
    sched_sleep_current_until(time_ticks() + 1u); return -EAGAIN;
}

/** @brief Model timed blocking until the next scheduler tick.
 * @param wake_tick Expected absolute wake tick; the production trap owns writer.
 */
void sched_sleep_current_until(uint64_t wake_tick)
{
    assert(writer_owned() && wake_tick == 1 && live_frame && live_frame->rip == RELIEFNT_USER_BASE + 0x1000);
    current->state = TASK_BLOCKED;
    current->wake_tick = wake_tick;
    ++gate_sleeps;
}
#endif

/** @brief Expose the current task as the only scheduler-table owner.
 * @param index Task-table index. @return Current task or NULL after it.
 */
#ifndef PRESSURE_CUSTOM_TASK_TABLE
#ifdef PRESSURE_REPORTING_FIXTURE
static struct task *reporting_peer;
struct task *sched_task_at(uint32_t index)
{ return index==0 ? current : index==1 ? reporting_peer : NULL; }
#else
struct task *sched_task_at(uint32_t index) { return index ? NULL : current; }
#endif
#endif

/** @brief No futex waiters exist in the trap fixture.
 * @param physical Candidate frame. @return False.
 */
#ifndef PRESSURE_REPORTING_FIXTURE
bool futex_swap_pinned(uint64_t physical) { (void)physical; return false; }
#endif

/** @brief Snapshot a host frame's fixture PMM reference count.
 * @param phys Aligned physical frame. @return Current count, or zero if free.
 */
uint32_t mm_page_refcount(uint64_t phys)
{
    uint32_t result = 0;
    pthread_mutex_lock(&page_mutex);
    for (unsigned i = 0; i < FAULT_FIXTURE_PAGES; ++i)
        if (pages[i].phys == phys) { result = pages[i].refs; break; }
    pthread_mutex_unlock(&page_mutex);
    return result;
}

/** @brief Release one fixture frame under its reference lock.
 * @param phys Owned physical frame. @return True when the frame became free.
 */
bool mm_free_page_reclaimed(uint64_t phys)
{
    pthread_mutex_lock(&page_mutex);
    unsigned i;
    for (i = 0; i < FAULT_FIXTURE_PAGES && (!pages[i].refs || pages[i].phys != phys); ++i) { }
    assert(i < FAULT_FIXTURE_PAGES);
    bool freed = !--pages[i].refs;
    if (freed) { free((void *)(uintptr_t)phys); pages[i].phys = 0; --live_pages; }
    pthread_mutex_unlock(&page_mutex);
    return freed;
}

/** @brief Exercise the backend's reentrant gate contract around a memory disk.
 * @param area Retained fixture swap area. @param page Logical slot page.
 * @param physical Caller-owned physical frame. @param write Direction.
 * @return Zero; assertions check actual writer depth, IRQs and fault spinlock.
 */
int swap_backend_page_io(struct swap_area *area, uint32_t page,
                         uint64_t physical, bool write)
{
    assert(area && page < GATE_MEDIA_PAGES && physical);
    assert(writer_owned() && execution_depth[cpu] == 1 && !reader_owned());
    assert(!irq_flags && !user_page_fault_lock.state);
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    assert(writer_owned() && execution_depth[cpu] == 2 && !irq_flags);
    if (!write && gate_contended) {
        kernel_execution_unlock_irqrestore(flags);
        return SWAP_STORAGE_CONTENDED;
    }
    if (write) { memcpy(gate_media[page], mm_phys_to_virt(physical), 4096); ++gate_writes; }
    else { memcpy(mm_phys_to_virt(physical), gate_media[page], 4096); ++gate_reads; }
    kernel_execution_unlock_irqrestore(flags);
    assert(execution_depth[cpu] == 1 && !irq_flags);
    return 0;
}

/** @brief Verify user-trap page-in occurs outside the fault spinlock.
 * @return Zero after byte preservation, lock and scheduling assertions hold.
 */
#ifndef SWAP_FAULT_GATE_FIXTURE_ENTRY
#define SWAP_FAULT_GATE_FIXTURE_ENTRY main
#endif
int SWAP_FAULT_GATE_FIXTURE_ENTRY(void)
{
    nx_enabled = true;
    page_cache_init();
    uint8_t header[4096] = {0}; header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *task = new_task();
    const uint64_t va = RELIEFNT_USER_BASE + 0x50000;
    task->vmas[0] = (struct task_vma){.used = 1, .start = va, .end = va + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    uint64_t frame = mm_alloc_page();
    assert(frame);
    memset(mm_phys_to_virt(frame), 0x6d, 4096);
    assert(address_space_map_user_page(sched_task_as(task), va, frame,
                                       RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    cpu = 0; current = task; current_task[0] = task; current_pid[0] = task->pid;
    uint64_t irq;
    kernel_execution_lock_irqsave(&irq);
    assert(swap_try_evict(sched_task_as(task), va) == 1);
    kernel_execution_unlock_irqrestore(irq);
    struct fault_work work = {task, va, 6, 0x23, 0, NULL, 0};
    gate_contended = true;
    run_fault(&work);
    assert(!atomic_load(&signals) && gate_sleeps == 1 && task->state == TASK_BLOCKED);
    assert(paging_user_entry_kind(sched_task_as(task), va) == PAGING_USER_ENTRY_SWAP);
    assert(!writer_owned() && work.result == &next_task);
    gate_contended = false; /* Model the other owner completing before wake. */
    run_fault(&work);
    frame = address_space_user_page_phys(sched_task_as(task), va);
    assert(frame && address_space_user_page_writable(sched_task_as(task), va));
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i] == 0x6d);
    assert(gate_reads == 1 && gate_writes == 1 && !atomic_load(&signals));
    assert(work.result == &next_task && !writer_owned() && !user_page_fault_lock.state && !irq_flags);
    destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
    puts("PASS real user trap: contention schedules, then page-in preserves bytes and gate boundaries");
    return 0;
}

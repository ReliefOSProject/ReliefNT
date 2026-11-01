/* Focused unpublished fork COW allocation failures: real scheduler, PTEs and
 * swap slots; modeled PMM admission and storage, with real swap eviction. */
#define TASK12_FORK_RECLAIM_FIXTURE 1
#define TASK8_FIX3_DEVICE_FORK 1
#include "task8_clone_tid_real_test.c"
#include "../../mm/swap/internal.h"

static uint8_t media[8][4096];
static struct task *fixture_parent, *fixture_child;
static uint64_t victims[2];
static unsigned reclaim_calls, scope_depth, pressure_limit, victims_evicted;
static uint64_t fail_heap_size;
static unsigned heap_denials;
static bool force_wait;

bool task12_fork_heap_admit(uint64_t size)
{
    if (fail_heap_size && size == fail_heap_size) {
        fail_heap_size = 0; ++heap_denials; return false;
    }
    return true;
}

uint64_t mm_emergency_reserve_pages(void) { return 64; }
uint64_t mm_free_memory_kib(void)
{
    unsigned ordinary = pressure_limit ?
        (pressure_limit > live_pages() ? pressure_limit - live_pages() : 0) : 128;
    return 4 * (64 + ordinary);
}
uint32_t mm_page_refcount(uint64_t physical)
{
    for (unsigned i = 0; i < 1024; ++i)
        if (pages[i].refs && pages[i].address == physical) return pages[i].refs;
    return 0;
}
bool mm_free_page_reclaimed(uint64_t physical)
{ bool released = mm_page_refcount(physical) == 1; mm_free_page(physical); return released; }
bool futex_swap_pinned(uint64_t physical) { (void)physical; return false; }
int swap_backend_page_io(struct swap_area *area, uint32_t page, uint64_t physical, bool write)
{
    assert(area && page < 8 && physical && !scheduler_lock.state);
    if (write) memcpy(media[page], mm_phys_to_virt(physical), 4096);
    else memcpy(mm_phys_to_virt(physical), media[page], 4096);
    return 0;
}
void sched_pressure_reset(struct task *task) { assert(task == fixture_parent); }
void memory_reclaim_fault_scope_begin(struct task *task, uint64_t rip, uint64_t address)
{
    assert(task == fixture_parent && rip && address && kernel_execution_is_outermost());
    assert(!scheduler_lock.state && !scope_depth); ++scope_depth;
}
void memory_reclaim_fault_scope_end(void) { assert(scope_depth == 1); --scope_depth; }
int memory_try_reclaim(uint32_t required, uint32_t flags)
{
    assert(scope_depth == 1 && !scheduler_lock.state && flags == 0);
    assert(fixture_child->state == TASK_EXITED && !sched_task_as(fixture_child)->cr3);
    struct fork_cow_reclaim_state *state = &((struct sched_task_object *)fixture_parent)->fork_cow_reclaim;
    assert(state->active && required >= state->minimum_pages && required > state->prefix_pages);
    ++reclaim_calls;
    if (force_wait) {
        assert(required > mm_free_memory_kib() / 4 - mm_emergency_reserve_pages());
        shm_physical_budget = -1;
        pressure_limit = 0;
        if (victims_evicted < 2) {
            assert(swap_try_evict(sched_task_as(fixture_parent), victims[victims_evicted]) == 1);
            ++victims_evicted;
        }
        force_wait = false;
        return -EAGAIN;
    }
    return 0;
}

static uint32_t slot_refs(struct swap_area *area, uint64_t token)
{
    uint32_t page = (uint32_t)token;
    return area->blocks[page / SWAP_SLOTS_PER_BLOCK]->slots[page % SWAP_SLOTS_PER_BLOCK].refs;
}

int main(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = 7;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 8, 1, &area));
    assert(!swap_area_publish(area));

    struct sched_task_object parent_object = {0}, child_object = {0};
    struct task *parent = &parent_object.task, *child = &child_object.task;
    fixture_parent = parent; fixture_child = child;
    task_zero(parent); task_zero(child); task_init_limits(parent);
    parent->pid = parent->tgid = 801; parent->kind = TASK_KIND_USER;
    parent->state = TASK_RUNNING; parent->running_cpu = 0;
    child->state = TASK_EXITED; child->running_cpu = SCHED_CPU_NONE;
    child->flags = TASK_FLAG_RESOURCES_RELEASED;
    struct task *entries[] = {parent, child};
    tasks = entries; task_count = task_capacity = 2;
    current_pid[0] = parent->pid; current_task[0] = parent; next_pid = 1;
    nx_enabled = true; empty_clone_boundary = true;
    assert(address_space_create(sched_task_as(parent)));
    const uint64_t base = 0x20000000, swapped = base + 4096;
    victims[0] = base + 8192; victims[1] = base + 0x200000;
    parent->vmas[0] = (struct task_vma){.used = 1, .start = base, .end = victims[1] + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    const uint64_t shared = 0x30000000;
    parent->vmas[1] = (struct task_vma){.used = 1, .start = shared, .end = shared + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_ANON};
    assert(!mm_shared_create(sched_task_as(parent), shared, shared + 4096));
    uint64_t addrs[] = {base, swapped, victims[0], victims[1]};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t page = mm_alloc_page(); assert(page);
        memset(mm_phys_to_virt(page), 0x70 + i, 4096);
        assert(address_space_map_user_page(sched_task_as(parent), addrs[i], page,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    }
    struct trap_frame frame = {.rax = LINUX_SYS_FORK, .rip = 0x400002, .rsp = 0x70000000};
    uint64_t irq; kernel_execution_lock_irqsave(&irq);
    assert(swap_try_evict(sched_task_as(parent), swapped) == 1);
    uint64_t token = mm_swap_record_lookup(sched_task_as(parent), swapped);
    uint64_t original = address_space_user_page_flags(sched_task_as(parent), base);
    uint64_t frame_before = address_space_user_page_phys(sched_task_as(parent), base);
    unsigned pages_before = live_pages();

    /* Actual root allocation shortage, followed by cancellation. */
    pressure_limit = live_pages(); shm_physical_budget = 0; force_wait = true;
    assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(parent_object.fork_cow_reclaim.active && reclaim_calls == 1);
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
    assert(live_pages() + 1 == pages_before);
    sched_fork_reclaim_cancel(parent);
    assert(!parent_object.fork_cow_reclaim.active && slot_refs(area, token) == 1);

    /* Five allocations reach the next region's actual staging-root failure,
     * after the first region has retained a swap slot and resident leaf. */
    pressure_limit = live_pages() + 5; shm_physical_budget = 5;
    force_wait = true;
    assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(parent_object.fork_cow_reclaim.active && reclaim_calls == 2);
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
    assert(live_pages() + 2 == pages_before);
    sched_fork_reclaim_cancel(parent);
    assert(!parent_object.fork_cow_reclaim.active && slot_refs(area, token) == 1);

    /* Child table branch: root and staging root succeed, next page fails. */
    pressure_limit = live_pages() + 2; shm_physical_budget = 2; force_wait = true;
    assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(parent_object.fork_cow_reclaim.active && reclaim_calls == 3);
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
    sched_fork_reclaim_cancel(parent);

    /* A registered writable alias forces a real copied-page allocation. */
    struct paging_writable_alias alias = {0};
    paging_writable_alias_add(&alias, frame_before);
    pressure_limit = live_pages() + 5; shm_physical_budget = 5; force_wait = true;
    assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(parent_object.fork_cow_reclaim.active && reclaim_calls == 4);
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
    sched_fork_reclaim_cancel(parent);
    paging_writable_alias_remove(&alias);

    /* Fail the actual child MM context, swap record and shared span allocations. */
    assert(sizeof(struct mm_context) != sizeof(struct mm_swap_record));
    assert(sizeof(struct mm_shared_span) != sizeof(struct mm_context));
    assert(sizeof(struct mm_shared_span) != sizeof(struct mm_swap_record));
    uint64_t heap_failures[] = {sizeof(struct mm_context), sizeof(struct mm_swap_record),
                                sizeof(struct mm_shared_span)};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned before = heap_denials;
        fail_heap_size = heap_failures[i]; pressure_limit = live_pages(); force_wait = true;
        assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(heap_denials == before + 1 && !fail_heap_size);
        assert(parent_object.fork_cow_reclaim.active && reclaim_calls == 5 + i);
        assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
        assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
        assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
        sched_fork_reclaim_cancel(parent);
    }

    /* Slot reference saturation is not a memory allocation failure. */
    uint32_t slot = (uint32_t)token;
    struct swap_slot *entry = &area->blocks[slot / SWAP_SLOTS_PER_BLOCK]->slots[slot % SWAP_SLOTS_PER_BLOCK];
    entry->refs = UINT32_MAX;
    unsigned before_calls = reclaim_calls;
    assert(sched_fork_current(&frame) == -LINUX_ENOMEM);
    assert(!parent_object.fork_cow_reclaim.active && reclaim_calls == before_calls);
    entry->refs = 1;
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);

    pressure_limit = live_pages() + 5; shm_physical_budget = 5;
    force_wait = true;
    assert(sched_fork_current(&frame) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(parent_object.fork_cow_reclaim.active && reclaim_calls == before_calls + 1);
    assert(child->state == TASK_EXITED && !sched_task_as(child)->cr3);
    assert(address_space_user_page_flags(sched_task_as(parent), base) == original);
    assert(mm_page_refcount(frame_before) == 1 && slot_refs(area, token) == 1);
    int64_t pid = sched_fork_current(&frame);
    assert(pid > 0 && child->pid == (uint32_t)pid && reclaim_calls == before_calls + 2);
    assert(!parent_object.fork_cow_reclaim.active && slot_refs(area, token) == 2);
    assert(address_space_user_page_flags(sched_task_as(parent), base) & RELIEFNT_PAGE_COW);
    assert(address_space_handle_cow_fault(sched_task_as(child), base));
    uint64_t copy = address_space_user_page_phys(sched_task_as(child), base);
    assert(copy && copy != frame_before && *(uint8_t *)mm_phys_to_virt(copy) == 0x70);
    *(uint8_t *)mm_phys_to_virt(copy) = 0x33;
    assert(*(uint8_t *)mm_phys_to_virt(frame_before) == 0x70);
    assert(swap_resolve_fault(child, swapped, 4) == 1);
    uint64_t restored = address_space_user_page_phys(sched_task_as(child), swapped);
    assert(restored && *(uint8_t *)mm_phys_to_virt(restored) == 0x71);
    assert(slot_refs(area, token) == 1);

    address_space_destroy(sched_task_as(child)); sched_task_vma_release(child);
    sched_task_fs_release(child); task_groups_release(child); syscall_release_task_files(child);
    kernel_free(child->exec_argv); kernel_free(child->exec_envp); kernel_free(child->exec_data);
    sched_task_identity_detach(child);
    address_space_destroy(sched_task_as(parent));
    assert(!live_pages() && !swap_area_unpublish(area)); swap_area_destroy(area);
    current_task[0] = NULL; current_pid[0] = 0; tasks = NULL; task_count = task_capacity = 0;
    kernel_execution_unlock_irqrestore(irq);
    puts("PASS fork COW allocation shortages: rollback, reclaim, cancellation, COW and swap references");
    return 0;
}

/* Real eviction and root metadata, ordinary heap failure, host-backed reserve. */
#define SWAP_PAGING_FIXTURE_ENTRY swap_paging_fixture_entry
#include "swap_paging_test.c"

/** @brief A previously unseen root must also survive ordinary heap exhaustion. */
static void without_existing_context(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *task = calloc(1, sizeof(*task)); assert(task);
    task->kind = TASK_KIND_USER; task->pid = 4; current_task = task;
    const uint64_t page = RELIEFNT_USER_BASE + 0x60000;
    task->vmas[0] = (struct task_vma){.used=1,.start=page,.end=page+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    uint64_t frame = mm_alloc_page(); assert(frame);
    memset(mm_phys_to_virt(frame), 0x39, 4096);
    assert(address_space_map_user_page(sched_task_as(task), page, frame,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(!mm_context_generation_existing(sched_task_as(task)));
    fail_kernel_alloc = true;
    int rc = swap_try_evict(sched_task_as(task), page);
    printf("first-MM eviction with ordinary metadata allocation denied=%d\n", rc); fflush(stdout);
    assert(rc == 1);
    fail_kernel_alloc = false;
    assert(swap_resolve_fault(task, page, 4) == 1);
    frame = address_space_user_page_phys(sched_task_as(task), page); assert(frame);
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i] == 0x39);
    address_space_destroy(sched_task_as(task)); current_task = NULL; free(task);
    assert(!allocated && !swap_area_unpublish(area)); swap_area_destroy(area);
}

int main(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *task = calloc(1, sizeof(*task)); assert(task);
    task->kind = TASK_KIND_USER; task->pid = 3; current_task = task; nx_enabled = true;
    const uint64_t page = RELIEFNT_USER_BASE + 0x50000;
    task->vmas[0] = (struct task_vma){.used=1,.start=page,.end=page+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    uint64_t generation;
    assert(!mm_context_ensure_generation(sched_task_as(task), &generation));
    uint64_t frame = mm_alloc_page(); assert(frame);
    memset(mm_phys_to_virt(frame), 0x6d, 4096);
    assert(address_space_map_user_page(sched_task_as(task), page, frame,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    fail_kernel_alloc = true;
    int rc = swap_try_evict(sched_task_as(task), page);
    printf("eviction with ordinary metadata allocation denied=%d\n", rc); fflush(stdout);
    assert(rc == 1);
    fail_kernel_alloc = false;
    assert(swap_resolve_fault(task, page, 4) == 1);
    frame = address_space_user_page_phys(sched_task_as(task), page); assert(frame);
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i] == 0x6d);
    address_space_destroy(sched_task_as(task)); current_task = NULL; free(task);
    assert(!allocated && !swap_area_unpublish(area)); swap_area_destroy(area);
    without_existing_context();
    puts("PASS emergency swap-record metadata is reclaimed with its last owner");
    return 0;
}

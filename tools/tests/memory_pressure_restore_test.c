/* Use production PTE, MM-record and swap transitions with modeled RAM/disk. */
#define SWAP_PAGING_FIXTURE_ENTRY swap_paging_fixture_entry
#include "swap_paging_test.c"

extern int swap_restore_leaf(struct task *, uint64_t) __attribute__((weak));

/** @brief Evacuating PROT_NONE data keeps its permissions and durable copy. */
int main(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->kind = TASK_KIND_USER;
    current_task = task;
    nx_enabled = true;
    const uint64_t page = RELIEFNT_USER_BASE + 0x31000;
    task->vmas[0] = (struct task_vma){.used=1,.start=page,.end=page+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    uint64_t frame = mm_alloc_page(); assert(frame);
    for (unsigned i = 0; i < 4096; ++i) ((uint8_t *)mm_phys_to_virt(frame))[i] = (uint8_t)(i * 13u + 7u);
    assert(address_space_map_user_page(sched_task_as(task), page, frame,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(swap_try_evict(sched_task_as(task), page) == 1);
    assert(address_space_protect_user_page(sched_task_as(task), page,
                                          RELIEFNT_PAGE_PROTNONE|RELIEFNT_PAGE_NOEXEC));
    task->vmas[0].prot = LINUX_PROT_NONE;
    uint64_t token = mm_swap_record_lookup(sched_task_as(task), page);
    assert(token && swap_resolve_fault(task, page, 4) == -EACCES);
    fail_alloc = true;
    assert(swap_restore_leaf(task, page) == -ENOMEM);
    fail_alloc = false;
    assert(mm_swap_record_lookup(sched_task_as(task), page) == token);
    swap_media_error = -EIO; swap_media_partial_error = true;
    assert(swap_restore_leaf(task, page) == -EIO);
    swap_media_error = 0; swap_media_partial_error = false;
    assert(mm_swap_record_lookup(sched_task_as(task), page) == token);
    assert(paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP);
    struct swap_stats before; swap_get_stats(&before); assert(before.used == 1);
    int result = swap_restore_leaf ? swap_restore_leaf(task, page) : swap_resolve_fault(task, page, 4);
    printf("PROT_NONE evacuation result=%d\n", result);
    fflush(stdout);
    assert(result == 1);
    frame = address_space_user_page_phys(sched_task_as(task), page);
    assert(frame && paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_PROTNONE);
    assert(!address_space_user_page_readable(sched_task_as(task), page));
    for (unsigned i = 0; i < 4096; ++i)
        assert(((uint8_t *)mm_phys_to_virt(frame))[i] == (uint8_t)(i * 13u + 7u));
    const uint64_t missing = page + 4096;
    task->vmas[1] = (struct task_vma){.used=1,.start=missing,.end=missing+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    fail_alloc = true;
    assert(!user_range_ok(missing, 32) && !user_range_writable(missing, 32));
    assert(paging_user_entry_kind(sched_task_as(task), missing) == PAGING_USER_ENTRY_EMPTY);
    fail_alloc = false;
    puts("PASS checked kernel usercopy rejects allocation failure without touching user bytes");
    struct swap_stats stats; swap_get_stats(&stats); assert(!stats.used);
    address_space_destroy(sched_task_as(task));
    current_task = NULL; free(task);
    assert(!allocated && !swap_area_unpublish(area));
    swap_area_destroy(area);
    puts("PASS swapoff restoration preserves inaccessible page bytes and permissions");
    return 0;
}

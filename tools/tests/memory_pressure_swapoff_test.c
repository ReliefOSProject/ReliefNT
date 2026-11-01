/* Real slot, page-table and MM-record paths, with a host RAM/disk provider. */
#define SWAP_PAGING_FIXTURE_ENTRY swap_paging_fixture_entry
#include "swap_paging_test.c"

extern int swap_drain_step(struct swap_area *, struct swap_drain_cursor *,
                           uint32_t, uint32_t *) __attribute__((weak));

int main(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *task = calloc(1, sizeof(*task)); assert(task);
    task->kind = TASK_KIND_USER; task->pid = 3; current_task = task;
    nx_enabled = true;
    const uint64_t base = RELIEFNT_USER_BASE + 0x40000;
    task->vmas[0] = (struct task_vma){.used=1,.start=base,.end=base+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    for (unsigned j = 0; j < 2; ++j) {
        uint64_t frame = mm_alloc_page(); assert(frame);
        for (unsigned i = 0; i < 4096; ++i)
            ((uint8_t *)mm_phys_to_virt(frame))[i] = (uint8_t)(i * 11u + j * 61u);
        assert(address_space_map_user_page(sched_task_as(task), base+j*4096, frame,
                                           RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
        assert(swap_try_evict(sched_task_as(task), base+j*4096) == 1);
    }
    /* The second root has two references to the same slots and is already
     * exited, but has not yet released its MM. */
    struct task *child = calloc(1, sizeof(*child)); assert(child);
    child->kind = TASK_KIND_USER; child->pid = 4; child->state = TASK_EXITED;
    assert(address_space_clone_cow(sched_task_as(task), sched_task_as(child)));
    fixture_extra_task = child;
    assert(!swap_area_begin_drain(area));
    struct swap_drain_cursor cursor = {0};
    uint32_t work = 0;
    fail_after_allocation = 2;
    int rc = swap_drain_step ? swap_drain_step(area, &cursor, 32, &work) :
                               swap_area_unpublish(area);
    printf("partial live evacuation status=%d work=%u\n", rc, work); fflush(stdout);
    assert(rc == -ENOMEM && work <= 32);
    fail_after_allocation = 0;
    assert(paging_user_entry_kind(sched_task_as(task), base) == PAGING_USER_ENTRY_RESIDENT);
    assert(paging_user_entry_kind(sched_task_as(task), base+4096) == PAGING_USER_ENTRY_SWAP);
    swap_area_abort_drain(area);
    struct swap_stats stats; swap_get_stats(&stats); assert(stats.used == 2);
    assert(!swap_area_begin_drain(area));
    cursor = (struct swap_drain_cursor){0};
    unsigned batches = 0;
    do {
        rc = swap_drain_step(area, &cursor, 2, &work);
        assert(work <= 2 && ++batches < 20);
    } while (!rc);
    assert(rc == 1 && !area->published);
    for (unsigned owner = 0; owner < 2; ++owner)
        for (unsigned j = 0; j < 2; ++j) {
            struct task *check = owner ? child : task;
            uint64_t frame = address_space_user_page_phys(sched_task_as(check), base+j*4096);
            assert(frame);
            for (unsigned i = 0; i < 4096; ++i)
                assert(((uint8_t *)mm_phys_to_virt(frame))[i] == (uint8_t)(i * 11u + j * 61u));
        }
    address_space_destroy(sched_task_as(child));
    fixture_extra_task = NULL; free(child);
    address_space_destroy(sched_task_as(task));
    current_task = NULL; free(task); assert(!allocated);
    swap_area_destroy(area);
    printf("PASS partial rollback and complete referenced swapoff in %u bounded batches\n", batches);
    return 0;
}

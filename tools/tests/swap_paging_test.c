/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse the production paging/fault fixture and its physical-page model. */
#define main private_anon_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "private_anon_fault_test.c"
#pragma clang diagnostic pop
#undef main
#include "../../mm/swap/internal.h"
static uint8_t swap_media[4][4096];
static int swap_media_error;
static bool swap_media_partial_error;
static bool fixture_outer_writer = true;
static bool fixture_pin_during_write;
static bool fixture_futex_waiter;

/** @brief Model a futex queue retaining this frame's physical key.
 * @param physical Candidate aligned frame. @return True while a waiter exists.
 */
bool futex_swap_pinned(uint64_t physical) { (void)physical; return fixture_futex_waiter; }

/** @brief Reject a file-VMA split in the anonymous-only lock fixture.
 * @param dst Destination slice. @param src Source slice. @return Never returns.
 */
int sched_task_vma_retain_file(struct task_vma *dst, const struct task_vma *src)
{ (void)dst; (void)src; abort(); }
/** @brief Reject a device owner in the anonymous-only lock fixture.
 * @param mapping Device mapping. @return Never returns.
 */
int device_mapping_get(struct device_mapping *mapping) { (void)mapping; abort(); }
/** @brief Reject device metadata in the anonymous-only lock fixture.
 * @param mapping Device mapping. @return Never returns.
 */
const struct device_mapping_info *device_mapping_info(const struct device_mapping *mapping)
{ (void)mapping; abort(); }
/** @brief Reject device addressing in the anonymous-only lock fixture.
 * @param mapping Device mapping. @param offset Device byte offset. @return Never returns.
 */
uint64_t device_mapping_physical(struct device_mapping *mapping, uint64_t offset)
{ (void)mapping; (void)offset; abort(); }
/** @brief Reject a backing inode in the anonymous-only lock fixture. @param inode Inode. */
void storage_inode_retain(struct storage_inode_ref *inode) { (void)inode; abort(); }

/** @brief Expose the current fixture owner through the production iterator.
 * @param index Task-table index. @return Current task at index zero, else NULL.
 */
#ifndef PRESSURE_CUSTOM_TASK_TABLE
static struct task *fixture_extra_task;
struct task *sched_task_at(uint32_t index)
{ return index == 0 ? current_task : index == 1 ? fixture_extra_task : NULL; }
#endif

/** @brief Read fixture PMM reference counts without changing ownership.
 * @param physical Aligned fixture frame. @return Current count or zero.
 */
uint32_t mm_page_refcount(uint64_t physical)
{
    for (unsigned i = 0; i < sizeof(host_page_refs) / sizeof(host_page_refs[0]); ++i)
        if (host_page_refs[i].physical == physical) return host_page_refs[i].references;
    return 0;
}

/** @brief Release a fixture reference and report its final physical release.
 * @param physical Owned fixture frame. @return True when no reference remains.
 */
bool mm_free_page_reclaimed(uint64_t physical)
{
    bool freed = mm_page_refcount(physical) == 1;
    mm_free_page(physical);
    return freed;
}

/** @brief Admit the explicitly modeled outer execution-writer fixture.
 * @return True; actual lock ownership is covered by a separate lock fixture.
 */
bool kernel_execution_is_outermost(void) { return fixture_outer_writer; }

/** @brief Transfer a real slot to the fixture's memory-backed disk.
 * @param area Published fixture area. @param page Logical page index.
 * @param physical Caller-owned aligned frame. @param write Transfer direction.
 * @return Zero after the page is copied, without changing slot ownership.
 */
int swap_backend_page_io(struct swap_area *area, uint32_t page,
                         uint64_t physical, bool write)
{
    assert(area && page < 4 && physical);
    if (swap_media_error) {
        if (swap_media_partial_error) {
            if (write) memcpy(swap_media[page], mm_phys_to_virt(physical), 2048);
            else memcpy(mm_phys_to_virt(physical), swap_media[page], 2048);
        }
        return swap_media_error;
    }
    if (write && fixture_pin_during_write) mm_retain_page(physical);
    if (write) memcpy(swap_media[page], mm_phys_to_virt(physical), 4096);
    else memcpy(mm_phys_to_virt(physical), swap_media[page], 4096);
    return 0;
}

/** @brief Verify fork retains a nonresident slot until the final MM exits.
 * @return None. Uses the real slot allocator and page-table clone/destruction;
 * only the disk transfer is replaced by the fixture above.
 */
static void test_swap_clone_lifetime(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1;
    header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    uint64_t token = swap_slot_alloc();
    uint64_t frame = mm_alloc_page();
    assert(token && frame);
    memset(mm_phys_to_virt(frame), 0xa7, 4096);
    assert(!swap_slot_submit_write(token, frame));
    mm_free_page(frame);

    struct task *owner = calloc(1, sizeof(*owner));
    assert(owner);
    owner->kind = TASK_KIND_USER;
    struct address_space *parent = sched_task_as(owner), child = {0};
    uint64_t page = RELIEFNT_USER_BASE + 0x8000;
    assert(address_space_create(parent));
    assert(address_space_prepare_user_range(parent, page, page + 4096));
    assert(!mm_swap_record_insert(parent, page, token));
    uint64_t *leaf = address_space_pte(parent, page);
    assert(leaf && !*leaf);
    *leaf = RELIEFNT_PAGE_USER | RELIEFNT_PAGE_SWAP;
    const uint64_t destination = page + 0x10000;
    assert(!task_remap_stage_leaves(owner, page, destination, 4096));
    assert(paging_user_entry_kind(parent, destination) == PAGING_USER_ENTRY_SWAP);
    task_unmap_pages(owner, page, page + 4096);
    assert(paging_user_entry_kind(parent, page) == PAGING_USER_ENTRY_EMPTY);
    page = destination;
    struct address_space failed_child = {0};
    fail_alloc = true;
    assert(!address_space_clone_cow(parent, &failed_child));
    fail_alloc = false;
    assert(paging_user_entry_kind(parent, page) == PAGING_USER_ENTRY_SWAP);
    assert(address_space_clone_cow(parent, &child));
    assert(paging_user_entry_kind(&child, page) == PAGING_USER_ENTRY_SWAP);
    task_unmap_pages(owner, page, page + 4096);
    assert(paging_user_entry_kind(parent, page) == PAGING_USER_ENTRY_EMPTY);
    address_space_destroy(parent);
    free(owner);
    assert(swap_area_unpublish(area) == -EBUSY);
    frame = mm_alloc_page();
    assert(frame && !swap_slot_submit_read(token, frame));
    for (unsigned i = 0; i < 4096; ++i)
        assert(((uint8_t *)mm_phys_to_virt(frame))[i] == 0xa7);
    mm_free_page(frame);
    address_space_destroy(&child);
    assert(!swap_area_unpublish(area));
    swap_area_destroy(area);
    assert(!allocated);
    puts("PASS fork preserves swap slot bytes and last-owner lifetime");
}

/** @brief Check an exclusive anonymous page is written and restored byte-exactly.
 * @return None. The real slot allocator owns the full opaque token throughout.
 */
static void test_swap_roundtrip(void)
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
    const uint64_t page = RELIEFNT_USER_BASE + 0x30000;
    task->vmas[0] = (struct task_vma){.used = 1, .start = page, .end = page + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    uint64_t frame = mm_alloc_page();
    assert(frame);
    for (unsigned i = 0; i < 4096; ++i) ((uint8_t *)mm_phys_to_virt(frame))[i] = (uint8_t)(i * 37u + 9u);
    assert(address_space_map_user_page(sched_task_as(task), page, frame,
                                       RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    fixture_outer_writer = false;
    assert(swap_try_evict(sched_task_as(task), page) == -EDEADLK);
    fixture_outer_writer = true;
    fixture_futex_waiter = true;
    assert(!swap_try_evict(sched_task_as(task), page));
    assert(address_space_user_page_phys(sched_task_as(task), page) == frame);
    fixture_futex_waiter = false;
    mm_retain_page(frame);
    assert(!swap_try_evict(sched_task_as(task), page));
    assert(mm_page_refcount(frame) == 2);
    mm_free_page(frame);
    task->vmas[0].flags |= TASK_VMA_FLAG_LOCKED;
    assert(!swap_try_evict(sched_task_as(task), page));
    task->vmas[0].flags &= ~TASK_VMA_FLAG_LOCKED;
    task->vmas[0].flags |= TASK_VMA_FLAG_SHARED;
    assert(!swap_try_evict(sched_task_as(task), page));
    task->vmas[0].flags &= ~TASK_VMA_FLAG_SHARED;
    task->vmas[0].flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_SHARED_FILE;
    assert(!swap_try_evict(sched_task_as(task), page));
    task->vmas[0].flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON;
    struct mm_lock_intent intent = {0};
    assert(!mm_context_lock_intent_begin(sched_task_as(task), page, page + 4096, &intent));
    assert(!swap_try_evict(sched_task_as(task), page));
    mm_context_lock_intent_end(&intent);
    fixture_pin_during_write = true;
    assert(!swap_try_evict(sched_task_as(task), page));
    fixture_pin_during_write = false;
    assert(address_space_user_page_phys(sched_task_as(task), page) == frame);
    assert(mm_page_refcount(frame) == 2);
    mm_free_page(frame);
    swap_media_error = -EIO;
    swap_media_partial_error = true;
    assert(swap_try_evict(sched_task_as(task), page) == -EIO);
    swap_media_error = 0; swap_media_partial_error = false;
    assert(address_space_user_page_phys(sched_task_as(task), page) == frame);
    assert(address_space_user_page_writable(sched_task_as(task), page));
    for (unsigned i = 0; i < 4096; ++i)
        assert(((uint8_t *)mm_phys_to_virt(frame))[i] == (uint8_t)(i * 37u + 9u));
    struct swap_stats stats;
    swap_get_stats(&stats);
    assert(!stats.used);
    int reclaimed = swap_try_evict(sched_task_as(task), page);
    assert(reclaimed == 1);
    assert(!mm_page_refcount(frame));
    assert(paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP);
    swap_get_stats(&stats);
    assert(stats.used == 1);
    assert(address_space_protect_user_page(sched_task_as(task), page,
                                          RELIEFNT_PAGE_PROTNONE | RELIEFNT_PAGE_NOEXEC));
    task->vmas[0].prot = LINUX_PROT_NONE;
    assert(swap_resolve_fault(task, page, 6) == -EACCES);
    assert(paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP);
    assert(address_space_protect_user_page(sched_task_as(task), page,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    task->vmas[0].prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    fail_alloc = true;
    assert(swap_resolve_fault(task, page, 6) == -ENOMEM);
    fail_alloc = false;
    swap_media_error = -EIO;
    swap_media_partial_error = true;
    assert(swap_resolve_fault(task, page, 6) == -EIO);
    swap_media_error = 0; swap_media_partial_error = false;
    swap_get_stats(&stats);
    assert(stats.used == 1);
    assert(paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP);
    fixture_outer_writer = false;
    assert(swap_prepare_user_range(task, page, 64, true) == -EDEADLK);
    fixture_outer_writer = true;
    assert(swap_prepare_user_range(task, page, 32769, true) == -EINVAL);
    int prepared = swap_prepare_user_range(task, page + 13, 64, true);
    assert(!prepared);
    frame = address_space_user_page_phys(sched_task_as(task), page);
    assert(frame && address_space_user_page_writable(sched_task_as(task), page));
    for (unsigned i = 0; i < 4096; ++i)
        assert(((uint8_t *)mm_phys_to_virt(frame))[i] == (uint8_t)(i * 37u + 9u));
    swap_get_stats(&stats);
    assert(stats.used == 0);
    assert(swap_try_evict(sched_task_as(task), page) == 1);
    task->cap_effective = UINT64_MAX;
    assert(!syscall_mm_mlock(page, 4096, 0));
    assert(address_space_user_page_phys(sched_task_as(task), page));
    assert(task->vmas[0].flags & TASK_VMA_FLAG_LOCKED);
    swap_get_stats(&stats);
    assert(!stats.used && !swap_try_evict(sched_task_as(task), page));
    address_space_destroy(sched_task_as(task));
    current_task = NULL;
    free(task);
    assert(!allocated && !swap_area_unpublish(area));
    swap_area_destroy(area);
    puts("PASS exclusive anonymous eviction and byte-exact fault-in");
}

/** @brief Fork and refault a slot with isolated writable copies, then a stack.
 * @return None. Disk bytes and slot references survive both independent faults.
 */
static void test_swap_fork_and_stack(void)
{
    uint8_t header[4096] = {0}; header[1024] = 1; header[1028] = 3;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), 4, 1, &area));
    assert(!swap_area_publish(area));
    struct task *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
    assert(parent && child); parent->kind = child->kind = TASK_KIND_USER;
    current_task = parent;
    const uint64_t va = RELIEFNT_USER_BASE + 0x70000;
    parent->vmas[0] = (struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    child->vmas[0] = parent->vmas[0];
    assert(address_space_create(sched_task_as(parent)));
    uint64_t phys = mm_alloc_page(); assert(phys); memset(mm_phys_to_virt(phys), 0x44, 4096);
    assert(address_space_map_user_page(sched_task_as(parent), va, phys, RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(swap_try_evict(sched_task_as(parent), va) == 1);
    assert(address_space_clone_cow(sched_task_as(parent), sched_task_as(child)));
    assert(swap_resolve_fault(child, va, 6) == 1);
    uint8_t *child_data = mm_phys_to_virt(address_space_user_page_phys(sched_task_as(child), va));
    assert(child_data && child_data[0] == 0x44); child_data[0] = 0x92;
    assert(swap_resolve_fault(parent, va, 6) == 1);
    uint8_t *parent_data = mm_phys_to_virt(address_space_user_page_phys(sched_task_as(parent), va));
    assert(parent_data && parent_data != child_data && parent_data[0] == 0x44);
    struct swap_stats stats; swap_get_stats(&stats); assert(!stats.used);
    address_space_destroy(sched_task_as(child)); address_space_destroy(sched_task_as(parent));
    free(child); free(parent);

    struct task *stack = calloc(1, sizeof(*stack)); assert(stack); stack->kind = TASK_KIND_USER;
    current_task = stack; stack->stack_low = RELIEFNT_USER_TOP - 3 * 4096;
    stack->stack_top = stack->stack_low + 4096;
    assert(address_space_create(sched_task_as(stack)));
    phys = mm_alloc_page(); assert(phys); memset(mm_phys_to_virt(phys), 0x5e, 4096);
    assert(address_space_map_user_page(sched_task_as(stack), stack->stack_low, phys,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(swap_try_evict(sched_task_as(stack), stack->stack_low) == 1);
    assert(swap_resolve_fault(stack, stack->stack_low, 6) == 1);
    assert(*(uint8_t *)mm_phys_to_virt(address_space_user_page_phys(sched_task_as(stack), stack->stack_low)) == 0x5e);
    address_space_destroy(sched_task_as(stack)); free(stack); current_task = NULL;
    assert(!allocated && !swap_area_unpublish(area)); swap_area_destroy(area);
    puts("PASS forked swap copies isolate writes; implicit stack roundtrip preserves bytes");
}

/** @brief Preserve an occupied nonpresent swap leaf on the anonymous fast path.
 * @return Zero after the leaf and its allocation count remain unchanged.
 * This classification-only case injects a nonpresent marker to test that an
 * occupied leaf is never zero-filled; the following tests own real slot tokens.
 */
#ifndef SWAP_PAGING_FIXTURE_ENTRY
#define SWAP_PAGING_FIXTURE_ENTRY main
#endif
int SWAP_PAGING_FIXTURE_ENTRY(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->kind = TASK_KIND_USER;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(task)));
    const uint64_t va = RELIEFNT_USER_BASE + 4096;
    task->vmas[0] = (struct task_vma){.used = 1, .start = va, .end = va + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE, .flags = TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(task), va, va + 4096));
    uint64_t *leaf = address_space_pte(sched_task_as(task), va);
    const uint64_t swap_marker = RELIEFNT_PAGE_USER | (1ULL << 53);
    assert(leaf && !*leaf);
    *leaf = swap_marker;
    unsigned before = allocated;
    assert(paging_user_entry_kind(sched_task_as(task), va) == PAGING_USER_ENTRY_SWAP);
    assert(!syscall_handle_private_anon_fault(task, va, 6));
    assert(*leaf == swap_marker);
    assert(allocated == before);
    assert(!address_space_user_range_free(sched_task_as(task), va, va + 4096));
    *leaf |= RELIEFNT_PAGE_PROTNONE;
    assert(paging_user_entry_kind(sched_task_as(task), va) == PAGING_USER_ENTRY_SWAP);
    assert(address_space_user_resident_kib(sched_task_as(task)) == 0);
    assert(!syscall_handle_task_page_fault(task, va, 6));
    assert(*leaf == (swap_marker | RELIEFNT_PAGE_PROTNONE));
    assert(address_space_protect_user_page(sched_task_as(task), va,
                                           RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(paging_user_entry_kind(sched_task_as(task), va) == PAGING_USER_ENTRY_SWAP);
    assert(!address_space_user_page_phys(sched_task_as(task), va));
    assert((*leaf & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_PROTNONE)) == 0);
    assert(*leaf & RELIEFNT_PAGE_NOEXEC);
    assert(address_space_protect_user_page(sched_task_as(task), va,
                                           RELIEFNT_PAGE_PROTNONE | RELIEFNT_PAGE_NOEXEC));
    uint64_t physical = mm_alloc_page(), found_page = 0, found_physical = 0;
    assert(physical);
    assert(address_space_map_user_page(sched_task_as(task), va + 4096, physical, 0));
    assert(address_space_user_page_next(sched_task_as(task), va, va + 8192,
                                        &found_page, &found_physical, NULL));
    assert(found_page == va + 4096 && found_physical == physical);
    *leaf = 0;
    address_space_destroy(sched_task_as(task));
    free(task);
    assert(!allocated);
    puts("PASS anonymous fast path preserves occupied nonpresent leaf");
    test_swap_clone_lifetime();
    test_swap_roundtrip();
    test_swap_fork_and_stack();
    return 0;
}

/* Real fault dispatcher, user mapping, page cache, pressure cursor, wait queue,
 * scheduler gate and swap PTE/slot code. A finite host page pool, provider and
 * RAM-backed swap transport model the external allocator and storage. */
#define PRESSURE_FIX5_FIXTURE 1
#ifndef GATE_MEDIA_PAGES
#define GATE_MEDIA_PAGES 64
#endif
#define PRESSURE_INTEGRATION_FIXTURE_ENTRY old_pressure_integration_main
#include "memory_pressure_fault_integration_test.c"

static bool quota_enforced;
static int provider_mode;
static unsigned provider_oom, oom_calls;

/** @brief Enforce the modeled ordinary frame pool at each real PMM call.
 * @return True when this allocation fits the independently tracked quota.
 */
#ifdef PRESSURE_REGULAR_IO_FIXTURE
static unsigned regular_fixture_page_denials;
#endif
static bool pressure_fixture_page_admitted(void)
{
    bool admitted = !quota_enforced || live_pages < pressure_baseline_pages;
#ifdef PRESSURE_REGULAR_IO_FIXTURE
    if (!admitted) ++regular_fixture_page_denials;
#endif
    return admitted;
}

/** @brief Supply one temporary provider frame or a genuine backing error.
 * @return Zero, -ENOMEM when its ordinary scratch frame is unavailable, or
 * -EIO for the explicitly injected media error. Successful scratch is freed.
 */
static int pressure_fixture_provider_before_read(void)
{
    if (provider_mode == 2) return -EIO;
    if (provider_mode == 1 || provider_mode == 3) {
        uint64_t scratch[40] = {0};
        unsigned needed = provider_mode == 3 ? 40 : 1, got = 0;
        while (got < needed && (scratch[got] = mm_alloc_page())) ++got;
        for (unsigned i = 0; i < got; ++i) mm_free_page(scratch[i]);
        if (got != needed) { ++provider_oom; return -ENOMEM; }
    }
    return 0;
}

/** @brief Model an exhausted victim set after the real bounded cursor scan.
 * @param budget Remaining candidate budget. @return Terminal -ENOMEM.
 */
static int pressure_fixture_no_victim(uint32_t budget)
{ assert(budget && budget <= 32); ++oom_calls; return -ENOMEM; }

/** @brief Start a private memory disk with enough slots for a new table path. */
static struct swap_area *setup_area(void)
{
    uint8_t header[4096] = {0};
    header[1024] = 1; header[1028] = GATE_MEDIA_PAGES - 1;
    memcpy(header + 4086, "SWAPSPACE2", 10);
    struct swap_area *area = NULL;
    assert(!swap_area_create(header, sizeof(header), GATE_MEDIA_PAGES, 1, &area));
    assert(!swap_area_publish(area));
    return area;
}

/** @brief Make the fixture task current and prepare real root-generation state. */
static void prepare_task(struct task *task)
{
    cpu = 0; current = task; current_task[0] = task; current_pid[0] = task->pid;
    task->state = TASK_RUNNING; task->running_cpu = 0;
    uint64_t flags, generation = 0;
    kernel_execution_lock_irqsave(&flags);
    assert(!mm_context_ensure_reclaim_generation(sched_task_as(task), &generation));
    kernel_execution_unlock_irqrestore(flags);
    assert(generation);
}

/** @brief Map disjoint eligible pages with independently known bytes. */
static void create_victims(struct task *task, uint64_t base, unsigned count)
{
    assert(count <= 48);
    task->vmas[1] = (struct task_vma){.used = 1, .start = base, .end = base + count * 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    for (unsigned i = 0; i < count; ++i) {
        uint64_t phys = mm_alloc_page(); assert(phys);
        memset(mm_phys_to_virt(phys), (int)(0x40 + i), 4096);
        assert(address_space_map_user_page(sched_task_as(task), base + i * 4096, phys,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    }
}

/** @brief Restore every victim and verify all bytes and real slot ownership. */
static void restore_victims(struct task *task, uint64_t base, unsigned count)
{
    quota_enforced = false;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t page = base + i * 4096;
        if (paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP)
            assert(swap_resolve_fault(task, page, 4) == 1);
        uint64_t phys = address_space_user_page_phys(sched_task_as(task), page); assert(phys);
        for (unsigned j = 0; j < 4096; ++j)
            assert(((uint8_t *)mm_phys_to_virt(phys))[j] == 0x40 + i);
    }
    kernel_execution_unlock_irqrestore(flags);
}

/** @brief Retry the same actual user instruction until installed or bounded.
 * @param task Task with a validated target VMA. @param address Target page.
 * @param error x86 fault flags. @return Actual fault-dispatch invocations.
 */
static unsigned resolve_without_signal(struct task *task, uint64_t address, uint64_t error)
{
    struct fault_work work = {task, address, error, 0x23, 0, NULL, 0};
    unsigned attempts = 0;
    do {
        run_fault(&work);
        ++attempts;
        if (atomic_load(&signals)) {
            fprintf(stderr, "unexpected signal while resolving %llx after %u attempts\n",
                    (unsigned long long)address, attempts);
            fflush(stderr);
        }
        assert(!atomic_load(&signals));
        assert(attempts < 1200);
    } while (!address_space_user_page_phys(sched_task_as(task), address));
    assert(!task->waiting_queue && !writer_owned() && !user_page_fault_lock.state);
    return attempts;
}

/** @brief Show both a cold-cache frame shortage and provider scratch shortage
 * progress through real reclaim rather than turning a valid file fault into EIO.
 */
static void file_shortage(unsigned scratch_pages)
{
    struct swap_area *area = setup_area();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
        .first_cluster = 71, .volume_id = 3, .size = 4096};
    memset(file_bytes, 0x5a, 4096);
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t target = 0x20000000, victims = 0x30000000;
    struct task *task = file_task(source, node, target, 0, node.size,
        TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
        TASK_VMA_FLAG_SHARED_FILE, LINUX_PROT_READ | LINUX_PROT_EXEC);
    assert(address_space_prepare_user_range(sched_task_as(task), target, target + 4096));
    unsigned victim_count = scratch_pages == 40 ? 48 : 8;
    create_victims(task, victims, victim_count); prepare_task(task);
    provider_mode = scratch_pages == 40 ? 3 : scratch_pages ? 1 : 0;
    pressure_baseline_pages = live_pages + (scratch_pages == 40 ? 5 : scratch_pages);
    quota_enforced = true;
    unsigned attempts = resolve_without_signal(task, target, 4);
    uint64_t result = address_space_user_page_phys(sched_task_as(task), target); assert(result);
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)mm_phys_to_virt(result))[i] == 0x5a);
    assert(gate_writes && (!scratch_pages || provider_oom));
    if (scratch_pages == 40) assert(gate_writes > 32);
    restore_victims(task, victims, victim_count);
    assert(gate_reads && !atomic_load(&signals));
    destroy_task(task); page_cache_invalidate_node(&node); vfs_exec_source_put(source);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials && !live_pages);
    assert(!swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS file shortage provider_scratch=%u attempts=%u full page and swap bytes intact\n",
           scratch_pages, attempts);
}

/** @brief Crossing a new 2 MiB region must reclaim more than the rolled-back
 * data frame, retry, and preserve all evicted anonymous bytes.
 */
static void anonymous_boundary(void)
{
    struct swap_area *area = setup_area();
    const uint64_t target = 0x60000000, victims = 0x30000000;
    struct task *task = new_task();
    task->vmas[0] = (struct task_vma){.used = 1, .start = target, .end = target + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    create_victims(task, victims, 8); prepare_task(task);
    pressure_baseline_pages = live_pages + 1;
    quota_enforced = true;
    unsigned attempts = resolve_without_signal(task, target, 6);
    uint64_t result = address_space_user_page_phys(sched_task_as(task), target); assert(result);
    for (unsigned i = 0; i < 4096; ++i) assert(!((uint8_t *)mm_phys_to_virt(result))[i]);
    assert(address_space_user_page_writable(sched_task_as(task), target) && gate_writes >= 4);
    restore_victims(task, victims, 8);
    assert(gate_reads >= 4 && !atomic_load(&signals));
    destroy_task(task); assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS new-table anonymous fault attempts=%u slots=%u original bytes intact\n", attempts, gate_reads);
}

/** @brief Genuine media failure and a denied write remain terminal, with no
 * pressure eviction or partially published cache/user mapping.
 */
static void negative_faults(void)
{
    struct swap_area *area = setup_area();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
        .first_cluster = 72, .volume_id = 3, .size = 4096};
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t target = 0x20000000;
    struct task *task = file_task(source, node, target, 0, node.size,
        TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
        TASK_VMA_FLAG_SHARED_FILE, LINUX_PROT_READ | LINUX_PROT_EXEC);
    assert(address_space_prepare_user_range(sched_task_as(task), target, target + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages + 1; quota_enforced = true;
    provider_mode = 2;
    struct fault_work work = {task, target, 4, 0x23, 0, NULL, 0};
    run_fault(&work);
    uint64_t alias = 0;
    assert(atomic_load(&signals) == 1 && !gate_writes &&
           !address_space_user_page_phys(sched_task_as(task), target) &&
           page_cache_lookup(&node, 0, &alias) < 0);
    task->vmas[0].prot = LINUX_PROT_READ;
    task->vmas[0].flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON;
    work.error = 6; run_fault(&work);
    assert(atomic_load(&signals) == 2 && !gate_writes &&
           !address_space_user_page_phys(sched_task_as(task), target));
    quota_enforced = false; destroy_task(task); vfs_exec_source_put(source);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    puts("PASS genuine EIO and denied write remain terminal without eviction");
}

/** @brief A nonproductive full scan terminates through the no-victim policy. */
static void no_victim(void)
{
    struct swap_area *area = setup_area();
    const uint64_t target = 0x60000000;
    struct task *task = new_task();
    task->vmas[0] = (struct task_vma){.used = 1, .start = target, .end = target + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    prepare_task(task); pressure_baseline_pages = live_pages + 1; quota_enforced = true;
    struct fault_work work = {task, target, 6, 0x23, 0, NULL, 0};
    unsigned attempts = 0;
    while (!atomic_load(&signals) && ++attempts < 1200) run_fault(&work);
    assert(attempts < 1200 && atomic_load(&signals) == 1 && oom_calls && !gate_writes &&
           !address_space_user_page_phys(sched_task_as(task), target));
    quota_enforced = false; destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS no-victim failure bounded after %u dispatches\n", attempts);
}

/** @brief Low-free counter jitter is not a reclaimed frame or a retry result. */
static void low_free_jitter(void)
{
    struct swap_area *area = setup_area();
    const uint64_t target = 0x20000000;
    struct task *task = new_task();
    task->vmas[0] = (struct task_vma){.used = 1, .start = target, .end = target + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(task), target, target + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages; quota_enforced = true;
    pressure_fixture_free_reads = 0; pressure_fixture_low_jitter = true;
    atomic_store(&fail_alloc, true);
    struct fault_work work = {task, target, 6, 0x23, 0, NULL, 0};
    unsigned attempts = 0, sleeps = 0;
    while (!atomic_load(&signals) && ++attempts < 1200) {
        run_fault(&work);
        if (task->state == TASK_BLOCKED) ++sleeps;
    }
    pressure_fixture_low_jitter = false;
    atomic_store(&fail_alloc, false);
    if (attempts >= 1200)
        fprintf(stderr, "low-free jitter repeatedly resumed failed allocation without progress\n");
    assert(attempts < 1200 && sleeps && atomic_load(&signals) == 1 && oom_calls &&
           !gate_writes && !address_space_user_page_phys(sched_task_as(task), target));
    quota_enforced = false; destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS low-free jitter reaches no-victim result after %u dispatches\n", attempts);
}

/** @brief With ample free pages, a persistent allocator failure blocks and
 * terminates in finite ticks, without spinning or evicting unrelated data.
 */
static void repeated_allocation_failure(void)
{
    struct swap_area *area = setup_area();
    const uint64_t target = 0x20000000;
    struct task *task = new_task();
    task->vmas[0] = (struct task_vma){.used = 1, .start = target, .end = target + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(task), target, target + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages + 10; quota_enforced = true;
    atomic_store(&fail_alloc, true);
    struct fault_work work = {task, target, 6, 0x23, 0, NULL, 0};
    unsigned attempts = 0, sleeps = 0;
    while (!atomic_load(&signals) && ++attempts < 1200) {
        /* An unrelated page can appear or disappear between retries. */
        pressure_baseline_pages = live_pages + 10 + (attempts & 1u);
        run_fault(&work);
        if (task->state == TASK_BLOCKED) ++sleeps;
    }
    atomic_store(&fail_alloc, false);
    assert(attempts < 1200 && attempts > 1 && sleeps && atomic_load(&signals) == 1 &&
           !gate_writes && !oom_calls && !address_space_user_page_phys(sched_task_as(task), target));
    quota_enforced = false; destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS persistent allocation failure waits then terminates after %u dispatches\n", attempts);
}

/** @brief An allocation that becomes available after timed waits resumes the
 * original instruction, without eviction, signal, or a retained wait link.
 */
static void transient_allocation_failure(void)
{
    struct swap_area *area = setup_area();
    const uint64_t target = 0x20000000;
    struct task *task = new_task();
    task->vmas[0] = (struct task_vma){.used = 1, .start = target, .end = target + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(task), target, target + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages + 10; quota_enforced = true;
    atomic_store(&fail_alloc, true);
    struct fault_work work = {task, target, 6, 0x23, 0, NULL, 0};
    for (unsigned i = 0; i < 3; ++i) {
        run_fault(&work);
        assert(!atomic_load(&signals) && task->state == TASK_BLOCKED && task->waiting_queue);
    }
    atomic_store(&fail_alloc, false);
    run_fault(&work);
    uint64_t frame = address_space_user_page_phys(sched_task_as(task), target);
    assert(frame && !task->waiting_queue && !atomic_load(&signals) && !gate_writes && !oom_calls);
    for (unsigned i = 0; i < 4096; ++i) assert(!((uint8_t *)mm_phys_to_virt(frame))[i]);
    quota_enforced = false; destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    puts("PASS transient allocation wait retries the original zero-filled page");
}

/** @brief Fill a cache frame with an independently selected byte pattern. */
static int fill_cache_frame(uint64_t phys, void *opaque)
{
    memset(mm_phys_to_virt(phys), *(const uint8_t *)opaque, 4096);
    return 0;
}

/** @brief Refresh-frame exhaustion must preserve the old frame and its data,
 * identify allocation failure, and merge only the newly valid suffix on retry.
 */
static void cache_refresh(void)
{
    struct storage_node old = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
        .first_cluster = 73, .volume_id = 3, .size = 2048};
    struct storage_node grown = old; grown.size = 4096;
    uint8_t first_byte = 0x2b, second_byte = 0x4c;
    uint64_t first = 0, result = 0;
    assert(!page_cache_load(&old, 0, fill_cache_frame, &first_byte, &first));
    page_cache_release(first);
    pressure_baseline_pages = live_pages; quota_enforced = true;
    int rc = page_cache_load(&grown, 0, fill_cache_frame, &second_byte, &result);
    if (rc != -ENOMEM) fprintf(stderr, "refresh allocation returned %d, expected -ENOMEM\n", rc);
    assert(rc == -ENOMEM && !result);
    uint64_t retained = 0;
    assert(!page_cache_lookup(&old, 0, &retained) && retained == first);
    for (unsigned i = 0; i < 2048; ++i) assert(((uint8_t *)mm_phys_to_virt(retained))[i] == first_byte);
    page_cache_release(retained); quota_enforced = false;
    assert(!page_cache_load(&grown, 0, fill_cache_frame, &second_byte, &result) && result == first);
    for (unsigned i = 0; i < 4096; ++i)
        assert(((uint8_t *)mm_phys_to_virt(result))[i] == (i < 2048 ? first_byte : second_byte));
    page_cache_release(result); page_cache_invalidate_node(&old);
    assert(!live_pages);
    puts("PASS failed refresh preserves old page and successful retry merges new suffix");
}

#ifndef PRESSURE_FIX5_FIXTURE_ENTRY
#define PRESSURE_FIX5_FIXTURE_ENTRY main
#endif
int PRESSURE_FIX5_FIXTURE_ENTRY(int argc, char **argv)
{
    assert(argc == 2); nx_enabled = true; page_cache_init();
    if (!strcmp(argv[1], "cache-frame")) file_shortage(0);
    else if (!strcmp(argv[1], "provider-memory")) file_shortage(1);
    else if (!strcmp(argv[1], "provider-large")) file_shortage(40);
    else if (!strcmp(argv[1], "anonymous-boundary")) anonymous_boundary();
    else if (!strcmp(argv[1], "negative")) negative_faults();
    else if (!strcmp(argv[1], "no-victim")) no_victim();
    else if (!strcmp(argv[1], "low-free-jitter")) low_free_jitter();
    else if (!strcmp(argv[1], "persistent-allocation")) repeated_allocation_failure();
    else if (!strcmp(argv[1], "transient-allocation")) transient_allocation_failure();
    else if (!strcmp(argv[1], "cache-refresh")) cache_refresh();
    else abort();
    return 0;
}

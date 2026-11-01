/* Real trap, cache and pressure paths, with modeled PMM and disk transport.
 * These controls count all cache eligibility checks during each dispatch. */
#define PRESSURE_REPORTING_FIXTURE 1
#define PRESSURE_FIX5_FIXTURE_ENTRY memory_fix5_fixture_main
#include "memory_pressure_fix5_test.c"
#include <reliefnt/memory_stats.h>
#include "../../kernel/ipc/futex.c"

static uintptr_t watched_eligibility;
static unsigned long eligibility_calls;
static bool measure_queries;
static unsigned long query_walks;
static bool measure_fault_context;
static unsigned forbidden_fault_calls;
static unsigned file_errors, terminal_faults;
bool storage_inode_swap_active(const struct storage_inode_ref *inode)
{ (void)inode; return false; }
static bool measure_cache;
static uint64_t cache_probes[5];
void page_cache_test_probe(unsigned kind)
{ if (measure_cache && kind < 5) ++cache_probes[kind]; }
static unsigned dirty_before_publication;
void paging_test_before_shared_write_publish(struct address_space *as, uint64_t page)
{
    uint64_t flags=address_space_user_page_flags(as,page);
    struct memory_snapshot snapshot; memory_snapshot_get(&snapshot);
    assert((flags&RELIEFNT_PAGE_WRITE_NOTIFY) && !(flags&RELIEFNT_PAGE_WRITABLE));
    assert(snapshot.dirty_kib >= 4);
    ++dirty_before_publication;
}

void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *callee, void *caller)
{
    (void)caller;
    if ((uintptr_t)callee == watched_eligibility) ++eligibility_calls;
    if (measure_queries && ((uintptr_t)callee == (uintptr_t)address_space_user_page_next ||
                            (uintptr_t)callee == (uintptr_t)sched_task_at)) ++query_walks;
    if (measure_fault_context &&
        ((uintptr_t)callee==(uintptr_t)mm_alloc_page ||
         (uintptr_t)callee==(uintptr_t)kernel_malloc ||
         (uintptr_t)callee==(uintptr_t)vfs_exec_source_read ||
         (uintptr_t)callee==(uintptr_t)page_cache_load ||
         (uintptr_t)callee==(uintptr_t)page_cache_load_fault ||
         (uintptr_t)callee==(uintptr_t)kernel_execution_suspend_irqrestore ||
         (uintptr_t)callee==(uintptr_t)sched_block_current ||
         (uintptr_t)callee==(uintptr_t)sched_sleep_current_until ||
         (uintptr_t)callee==(uintptr_t)userland_schedule_from_frame)) ++forbidden_fault_calls;
}

void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *callee, void *caller)
{ (void)callee; (void)caller; }

/** @brief Observe actual severity and originating format, without formatting. */
static void pressure_reporting_log(enum console_log_level level, const char *format)
{
    if (level == CONSOLE_LOG_ERROR && strstr(format, "lazy file map failed")) ++file_errors;
    if (level == CONSOLE_LOG_WARN && strstr(format, "user page fault signal=")) ++terminal_faults;
}

/** @brief A fault starting after other cache work must still reach its prefix. */
static void cache_prefix(void)
{
    uint32_t inspected = 0; bool complete = false;
    for (unsigned i = 0; i < 2; ++i) {
        assert(!page_cache_pressure_scan(32, &inspected, &complete));
        assert(inspected == 32 && !complete);
    }
    struct storage_node idle = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 901,
        .volume_id = 3, .size = 36 * 4096ULL};
    uint8_t fill = 0x33;
    for (unsigned i = 0; i < 36; ++i) {
        uint64_t frame = 0;
        assert(!page_cache_load(&idle, (uint64_t)i * 4096, fill_cache_frame, &fill, &frame));
        page_cache_release(frame);
    }
    assert(page_cache_reclaimable_pages() == 36);
    struct swap_area *area = setup_area();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 902,
        .volume_id = 3, .size = 4096};
    memset(file_bytes, 0x5a, 4096);
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t address = 0x20000000;
    struct task *task = file_task(source, node, address, 0, node.size,
        TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
        TASK_VMA_FLAG_SHARED_FILE, LINUX_PROT_READ | LINUX_PROT_EXEC);
    assert(address_space_prepare_user_range(sched_task_as(task), address, address + 4096));
    prepare_task(task); provider_mode = 3;
    pressure_baseline_pages = live_pages + 5; quota_enforced = true;
    unsigned attempts = resolve_without_signal(task, address, 4);
    uint64_t mapped = address_space_user_page_phys(sched_task_as(task), address);
    assert(mapped && provider_oom && !gate_writes);
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)mm_phys_to_virt(mapped))[i] == 0x5a);
    printf("PASS cache prefix resolved in %u dispatches with real cache frames\n", attempts);
    quota_enforced = false; destroy_task(task);
    page_cache_invalidate_node(&idle); page_cache_invalidate_node(&node);
    vfs_exec_source_put(source); assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
}

/** @brief Two interleaved faults must each see a complete shared-cache cycle. */
static void cache_interleaved(void)
{
    uint32_t inspected = 0; bool complete = false;
    assert(!page_cache_pressure_scan(32, &inspected, &complete));
    assert(inspected == 32 && !complete);
    struct storage_node idle = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 906,
        .volume_id = 3, .size = 36 * 4096ULL};
    uint8_t fill = 0x35;
    for (unsigned i = 0; i < 36; ++i) {
        uint64_t frame = 0;
        assert(!page_cache_load(&idle, (uint64_t)i * 4096, fill_cache_frame, &fill, &frame));
        page_cache_release(frame);
    }
    struct swap_area *area = setup_area();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 907,
        .volume_id = 3, .size = 4096};
    memset(file_bytes, 0x5a, 4096);
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t address = 0x20000000;
    uint32_t flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
                     TASK_VMA_FLAG_SHARED_FILE;
    struct task *a = file_task(source, node, address, 0, node.size, flags, LINUX_PROT_READ);
    struct task *b = file_task(source, node, address, 0, node.size, flags, LINUX_PROT_READ);
    b->pid = 124;
    assert(address_space_prepare_user_range(sched_task_as(a), address, address + 4096));
    assert(address_space_prepare_user_range(sched_task_as(b), address, address + 4096));
    prepare_task(a); prepare_task(b); provider_mode = 3;
    pressure_baseline_pages = live_pages + 5; quota_enforced = true;
    struct fault_work work[] = {{a, address, 4, 0x23, 0, NULL, 0},
                                {b, address, 4, 0x23, 0, NULL, 0}};
    unsigned attempts = 0;
    while ((!address_space_user_page_phys(sched_task_as(a), address) ||
            !address_space_user_page_phys(sched_task_as(b), address)) && ++attempts < 2400) {
        unsigned index = attempts & 1u;
        if (!address_space_user_page_phys(sched_task_as(work[index].task), address))
            run_fault(&work[index]);
        assert(!atomic_load(&signals));
    }
    if (attempts >= 2400 || !provider_oom || gate_writes)
        fprintf(stderr, "interleaved attempts=%u provider_oom=%u writes=%u mapped=%llu,%llu\n",
                attempts, provider_oom, gate_writes,
                (unsigned long long)address_space_user_page_phys(sched_task_as(a), address),
                (unsigned long long)address_space_user_page_phys(sched_task_as(b), address));
    assert(attempts < 2400 && provider_oom && !gate_writes);
    uint64_t pa = address_space_user_page_phys(sched_task_as(a), address);
    uint64_t pb = address_space_user_page_phys(sched_task_as(b), address);
    assert(pa && pb && *(uint8_t *)mm_phys_to_virt(pa) == 0x5a &&
           *(uint8_t *)mm_phys_to_virt(pb) == 0x5a);
    printf("PASS two interleaved faults resolved after %u dispatches\n", attempts);
    quota_enforced = false; destroy_task(a); destroy_task(b);
    page_cache_invalidate_node(&idle); page_cache_invalidate_node(&node);
    vfs_exec_source_put(source); assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
}

/** @brief Count cache candidates across allocation, fallback and pressure. */
static void dispatch_budget(bool refresh, uintptr_t eligibility)
{
    struct storage_node old = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 903,
        .volume_id = 3, .size = 2048};
    uint8_t fill = 0x45; uint64_t pin = 0;
    assert(!page_cache_load(&old, 0, fill_cache_frame, &fill, &pin));
    struct storage_node node = old;
    if (!refresh) node.first_cluster = 904;
    node.size = 4096;
    struct swap_area *area = setup_area();
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t address = 0x20000000;
    struct task *task = file_task(source, node, address, 0, node.size,
        TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
        TASK_VMA_FLAG_SHARED_FILE, LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task), address, address + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages; quota_enforced = true;
    atomic_store(&fail_alloc, true);
    struct fault_work work = {task, address, 4, 0x23, 0, NULL, 0};
    unsigned attempts = 0; unsigned long maximum = 0;
    watched_eligibility = eligibility;
    while (!atomic_load(&signals) && ++attempts < 1200) {
        eligibility_calls = 0; memset(cache_probes,0,sizeof(cache_probes));
        measure_cache=true; run_fault(&work); measure_cache=false;
        if (eligibility_calls > maximum) maximum = eligibility_calls;
        assert(cache_probes[4] <= 32 && cache_probes[3] <= 8);
    }
    watched_eligibility = 0;
    assert(attempts < 1200 && atomic_load(&signals) == 1 && terminal_faults == 1 && !gate_writes);
    printf("PASS %s predicate calls maximum=%lu attempts=%u terminal diagnostic=1\n",
           refresh ? "refresh" : "cold", maximum, attempts);
    atomic_store(&fail_alloc, false); quota_enforced = false;
    destroy_task(task); page_cache_release(pin); page_cache_invalidate_node(&old);
    vfs_exec_source_put(source); assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
}

/** @brief Count actual cache probes across a complete dispatch at fixed and
 * overflow capacity, then complete the same fault and check provider bytes. */
static void dispatch_full_capacity(void)
{
    enum { PINNED=4096+64 };
    uint64_t *frames=calloc(PINNED,sizeof(*frames)); assert(frames);
    struct storage_node pinned={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=930,.volume_id=3,.size=(uint64_t)PINNED*4096};
    uint8_t fill=0x19;
    for (unsigned i=0;i<PINNED;++i)
        assert(!page_cache_load_fault(&pinned,(uint64_t)i*4096,fill_cache_frame,&fill,&frames[i]));
    struct page_cache_report report; page_cache_report(&report); assert(report.resident==PINNED);
    struct swap_area *area=setup_area();
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=931,.volume_id=3,.size=4096};
    memset(file_bytes,0x5a,4096);
    struct vfs_exec_source *source=exec_fixture_open(&node); const uint64_t target=0x20000000;
    struct task *task=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+4096));
    prepare_task(task); pressure_baseline_pages=live_pages; quota_enforced=true;
    atomic_store(&fail_alloc,true);
    struct fault_work work={task,target,4,0x23,0,NULL,0};
    memset(cache_probes,0,sizeof(cache_probes)); measure_cache=true; run_fault(&work); measure_cache=false;
    assert(!atomic_load(&signals) && !address_space_user_page_phys(sched_task_as(task),target));
    assert(cache_probes[1]<512 && cache_probes[2]<512 && cache_probes[3]<=8 && cache_probes[4]<=32);
    uint64_t pending[5]; memcpy(pending,cache_probes,sizeof(pending));
    atomic_store(&fail_alloc,false); quota_enforced=false;
    memset(cache_probes,0,sizeof(cache_probes)); measure_cache=true; run_fault(&work); measure_cache=false;
    uint64_t mapped=address_space_user_page_phys(sched_task_as(task),target);
    assert(mapped && !atomic_load(&signals));
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(mapped))[i]==0x5a);
    assert(cache_probes[1]<512 && cache_probes[2]<512 && cache_probes[3]<=16 && cache_probes[4]<=32);
    printf("PASS 4160 pinned dispatch pending key_touches=%llu phys_touches=%llu availability_checks=%llu candidates=%llu; "
           "completed key_touches=%llu phys_touches=%llu availability_checks=%llu candidates=%llu bytes=4096\n",
           (unsigned long long)pending[1],(unsigned long long)pending[2],
           (unsigned long long)pending[3],(unsigned long long)pending[4],
           (unsigned long long)cache_probes[1],(unsigned long long)cache_probes[2],
           (unsigned long long)cache_probes[3],(unsigned long long)cache_probes[4]);
    destroy_task(task); page_cache_invalidate_node(&node); vfs_exec_source_put(source);
    for (unsigned i=0;i<PINNED;++i) page_cache_release(frames[i]);
    page_cache_forget_node(&pinned); free(frames);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
}

/** @brief Retriable ENOMEM must not log an error on every dispatch. */
static void retry_log(void)
{
    struct swap_area *area = setup_area();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_FAT32, .first_cluster = 905,
        .volume_id = 3, .size = 4096};
    struct vfs_exec_source *source = exec_fixture_open(&node);
    const uint64_t address = 0x20000000;
    struct task *task = file_task(source, node, address, 0, node.size,
        TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
        TASK_VMA_FLAG_SHARED_FILE, LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task), address, address + 4096));
    prepare_task(task); pressure_baseline_pages = live_pages + 5;
    quota_enforced = true; provider_mode = 3;
    struct fault_work work = {task, address, 4, 0x23, 0, NULL, 0};
    struct memory_events before, after; memory_events_get(&before);
    for (unsigned i = 0; i < 3; ++i) {
        run_fault(&work); assert(!atomic_load(&signals));
    }
    if (file_errors) fprintf(stderr, "transient retries emitted %u file errors\n", file_errors);
    assert(provider_oom == 3 && !file_errors);
    unsigned attempts = 3;
    while (!atomic_load(&signals) && ++attempts < 1200) run_fault(&work);
    assert(attempts < 1200 && terminal_faults == 1 && !file_errors);
    memory_events_get(&after);
    assert(after.value[MEMORY_EVENT_PGFAULT] == before.value[MEMORY_EVENT_PGFAULT] + 1);
    printf("PASS transient retries quiet and one terminal diagnostic after %u dispatches\n", attempts);
    quota_enforced = false; destroy_task(task); vfs_exec_source_put(source);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
}

/** @brief Genuine backing errors remain reported and terminal. */
static void backing_log(void)
{
    negative_faults();
    assert(file_errors == 1 && terminal_faults == 2);
    puts("PASS backing EIO diagnostic remains visible");
}

/** @brief Non-retrying mlock and task-usercopy can use one idle cache frame. */
static void prefault_shortage(bool copy)
{
    struct storage_node idle={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=910,.volume_id=3,.size=4096}, node=idle;
    node.first_cluster=911; uint8_t fill=0x37; uint64_t frame=0;
    assert(!page_cache_load(&idle,0,fill_cache_frame,&fill,&frame));
    page_cache_release(frame); assert(page_cache_reclaimable_pages()==1);
    memset(file_bytes,0x5a,4096);
    struct vfs_exec_source *source=exec_fixture_open(&node);
    const uint64_t target=0x20000000;
    struct task *task=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+4096));
    prepare_task(task); pressure_baseline_pages=live_pages; quota_enforced=true;
    uint64_t flags; kernel_execution_lock_irqsave(&flags); uint8_t bytes[16]={0};
    int rc=copy ? user_copy_from_task(task,bytes,target,sizeof(bytes)) :
                  task_populate_lock_vma_page(task,&task->vmas[0],target);
    if (rc) fprintf(stderr,"nonretry %s failed rc=%d idle=%llu\n",copy?"usercopy":"mlock",
                    rc,(unsigned long long)page_cache_reclaimable_pages());
    assert(!rc);
    uint64_t mapped=address_space_user_page_phys(sched_task_as(task),target); assert(mapped);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(mapped))[i]==0x5a);
    if(copy) for(unsigned i=0;i<sizeof(bytes);++i) assert(bytes[i]==0x5a);
    kernel_execution_unlock_irqrestore(flags); quota_enforced=false;
    destroy_task(task); page_cache_invalidate_node(&node); vfs_exec_source_put(source);
    assert(!live_pages); printf("PASS nonretry %s uses reclaimable idle frame\n",copy?"usercopy":"mlock");
}

/** @brief A non-retry ring-0 user fault retains the specific file diagnostic. */
static void ring0_log(void)
{
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=912,.volume_id=3,.size=4096};
    struct vfs_exec_source *source=exec_fixture_open(&node); const uint64_t target=0x20000000;
    struct task *task=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+4096));
    prepare_task(task); pressure_baseline_pages=live_pages; quota_enforced=true;
    atomic_store(&fail_alloc,true); uint64_t flags; kernel_execution_lock_irqsave(&flags);
    int rc=syscall_handle_user_page_fault_detailed(target,4,false);
    assert(rc==-ENOMEM && file_errors==1);
    kernel_execution_unlock_irqrestore(flags); atomic_store(&fail_alloc,false); quota_enforced=false;
    destroy_task(task);vfs_exec_source_put(source);assert(!live_pages);
    puts("PASS ring-0 allocation failure emits specific lazy-file diagnostic");
}

/** @brief Synchronous callers keep their real terminal shortage and EIO. */
static void prefault_terminal(void)
{
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=933,.volume_id=3,.size=4096};
    struct vfs_exec_source *source=exec_fixture_open(&node); const uint64_t target=0x20000000;
    struct task *task=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+4096));
    prepare_task(task); uint64_t lock; kernel_execution_lock_irqsave(&lock);
    atomic_store(&fail_alloc,true); uint8_t data=0;
    assert(task_populate_lock_vma_page(task,&task->vmas[0],target)==-ENOMEM);
    assert(user_copy_from_task(task,&data,target,1)==-EFAULT && file_errors==1);
    atomic_store(&fail_alloc,false); provider_mode=2;
    assert(task_populate_lock_vma_page(task,&task->vmas[0],target)==-EIO);
    assert(user_copy_from_task(task,&data,target,1)==-EFAULT && file_errors==2);
    assert(!address_space_user_page_phys(sched_task_as(task),target));
    kernel_execution_unlock_irqrestore(lock); destroy_task(task); vfs_exec_source_put(source);
    assert(!live_pages); puts("PASS nonretry terminal ENOMEM and EIO remain visible");
}

/** @brief Real fault, cache walk and completed swap I/O update event counters. */
static void actual_events(void)
{
    struct swap_area *area = setup_area();
    struct storage_node node = {.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=908,.volume_id=3,.size=2*4096};
    memset(file_bytes,0x5a,2*4096);
    struct vfs_exec_source *source=exec_fixture_open(&node);
    const uint64_t target=0x20000000,victims=0x30000000;
    struct task *task=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+4096));
    create_victims(task,victims,8);prepare_task(task);
    struct memory_events before,after;memory_events_get(&before);
    pressure_baseline_pages=live_pages;quota_enforced=true;
    unsigned attempts=resolve_without_signal(task,target,4);
    memory_events_get(&after);
    assert(attempts > 1 && after.value[MEMORY_EVENT_PGFAULT]==before.value[MEMORY_EVENT_PGFAULT]+1 &&
           after.value[MEMORY_EVENT_PGMAJFAULT]==before.value[MEMORY_EVENT_PGMAJFAULT]+1 &&
           after.value[MEMORY_EVENT_PSWPOUT]>before.value[MEMORY_EVENT_PSWPOUT] &&
           after.value[MEMORY_EVENT_PGSCAN_DIRECT]>before.value[MEMORY_EVENT_PGSCAN_DIRECT] &&
           after.value[MEMORY_EVENT_PGSTEAL_DIRECT]>before.value[MEMORY_EVENT_PGSTEAL_DIRECT]);
    quota_enforced=false;
    unsigned swapped=0;
    while (swapped<8 && paging_user_entry_kind(sched_task_as(task),victims+swapped*4096)!=PAGING_USER_ENTRY_SWAP) ++swapped;
    assert(swapped<8);
    struct fault_work work={task,victims+swapped*4096,4,0x23,0,NULL,0};
    run_fault(&work); memory_events_get(&after);
    assert(after.value[MEMORY_EVENT_PGFAULT]==before.value[MEMORY_EVENT_PGFAULT]+2 &&
           after.value[MEMORY_EVENT_PGMAJFAULT]==before.value[MEMORY_EVENT_PGMAJFAULT]+2 &&
           after.value[MEMORY_EVENT_PSWPIN]>before.value[MEMORY_EVENT_PSWPIN]);
    struct task *warm=file_task(source,node,target,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    prepare_task(warm); assert(resolve_without_signal(warm,target,4)==1);
    memory_events_get(&after);
    assert(after.value[MEMORY_EVENT_PGFAULT]==before.value[MEMORY_EVENT_PGFAULT]+3 &&
           after.value[MEMORY_EVENT_PGMAJFAULT]==before.value[MEMORY_EVENT_PGMAJFAULT]+2);
    destroy_task(warm);
    struct task *prefault=file_task(source,node,target,4096,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    prepare_task(prefault); uint64_t lock;
    kernel_execution_lock_irqsave(&lock);
    assert(syscall_handle_task_page_fault(prefault,target,4));
    kernel_execution_unlock_irqrestore(lock); memory_events_get(&after);
    assert(after.value[MEMORY_EVENT_PGFAULT]==before.value[MEMORY_EVENT_PGFAULT]+3 &&
           after.value[MEMORY_EVENT_PGMAJFAULT]==before.value[MEMORY_EVENT_PGMAJFAULT]+2);
    destroy_task(prefault);
    prepare_task(task);
    restore_victims(task,victims,8);memory_events_get(&after);
    assert(after.value[MEMORY_EVENT_PSWPIN]>before.value[MEMORY_EVENT_PSWPIN]);
    printf("PASS actual fault and completed page I/O events after %u dispatches\n",attempts);
    destroy_task(task);page_cache_invalidate_node(&node);vfs_exec_source_put(source);
    assert(!live_pages && !swap_area_unpublish(area));swap_area_destroy(area);
}

/** @brief Shared writable alone is unknown; real PTE and alias writes are dirty. */
static void dirty_cache(void)
{
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=909,.volume_id=3,.size=3*4096};
    const uint64_t base=0x30000000; memset(file_bytes,0x51,3*4096);
    struct vfs_exec_source *source=exec_fixture_open(&node);
    struct task *task=file_task(source,node,base,0,node.size,
        TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ|LINUX_PROT_WRITE);
    task->vmas[0].end=base+3*4096;
    uint64_t frame[3]={0};
    prepare_task(task); struct memory_snapshot snapshot; uint64_t lock;
    kernel_execution_lock_irqsave(&lock);
    for (unsigned i=0;i<3;++i) {
        assert(syscall_handle_task_page_fault(task,base+i*4096,4));
        frame[i]=address_space_user_page_phys(sched_task_as(task),base+i*4096);
        assert(frame[i] && !(address_space_user_page_flags(sched_task_as(task),base+i*4096) &
                              RELIEFNT_PAGE_WRITABLE));
    }
    memory_snapshot_get(&snapshot);
    assert(snapshot.cached_kib==12 && snapshot.dirty_kib==0);
    assert(syscall_handle_user_page_fault_detailed(base,7,true)==1);
    assert(address_space_user_page_flags(sched_task_as(task),base)&RELIEFNT_PAGE_WRITABLE);
    *(uint8_t *)mm_phys_to_virt(frame[0])=0x72;
    memory_snapshot_get(&snapshot); assert(snapshot.dirty_kib==4);
    assert(syscall_handle_user_page_fault_detailed(base+4096,7,true)==1);
    assert(dirty_before_publication==2);
    assert(address_space_unmap_user_page(sched_task_as(task),base+4096)==frame[1]);
    page_cache_release(frame[1]);
    assert(!user_copy_to_task(task,base+2*4096,"X",1));
    /* Unrelated dirty shared RAM and many aliases do not make queries visit
     * mapping leaves or count a non-cache physical frame as file dirty. */
    for (unsigned i=0;i<128;++i) {
        uint64_t extra=mm_alloc_page(); assert(extra);
        assert(address_space_map_user_page(sched_task_as(task),0x40000000+(uint64_t)i*4096,
               extra,RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_DIRTY));
    }
    for (unsigned i=0;i<32;++i) {
        assert(!page_cache_retain(frame[0]));
        assert(address_space_map_user_page(sched_task_as(task),0x50000000+(uint64_t)i*4096,
               frame[0],RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITE_NOTIFY));
    }
    struct task peer={.kind=TASK_KIND_USER,.pid=124,.state=TASK_RUNNING,
                      .shared_mm=&task->address_space};
    reporting_peer=&peer;
    assert(sched_task_as(&peer)==sched_task_as(task));
    memset(cache_probes,0,sizeof(cache_probes)); measure_cache=true;
    query_walks=0; measure_queries=true;
    for (unsigned i=0;i<100;++i) memory_snapshot_get(&snapshot);
    measure_cache=false; measure_queries=false; assert(snapshot.dirty_kib==12 && !query_walks);
    for (unsigned i=0;i<5;++i) assert(!cache_probes[i]);
    reporting_peer=NULL;
    kernel_execution_unlock_irqrestore(lock);
    destroy_task(task); page_cache_forget_node(&node); vfs_exec_source_put(source);
    assert(!live_pages);
    puts("PASS shared PTE and physical-alias writes counted, writable-only remains unknown");
}

/** @brief Permission-only changes, clone and private COW never fabricate dirty. */
static void dirty_policy(void)
{
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=932,.volume_id=3,.size=4096};
    memset(file_bytes,0x31,4096); struct vfs_exec_source *source=exec_fixture_open(&node);
    const uint64_t address=0x30000000;
    struct task *shared=file_task(source,node,address,0,node.size,
        TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ);
    shared->vmas[0].max_prot=LINUX_PROT_READ|LINUX_PROT_WRITE;
    prepare_task(shared); uint64_t lock; kernel_execution_lock_irqsave(&lock);
    assert(syscall_handle_task_page_fault(shared,address,4));
    struct memory_snapshot snapshot; memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    unsigned flushes=fixture_tlb_flushes;
    assert(!syscall_mm_mprotect(address,4096,LINUX_PROT_READ|LINUX_PROT_WRITE));
    assert(fixture_tlb_flushes>flushes);
    uint64_t flags=address_space_user_page_flags(sched_task_as(shared),address);
    assert((flags&RELIEFNT_PAGE_WRITE_NOTIFY) && !(flags&RELIEFNT_PAGE_WRITABLE));
    memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    struct address_space child={0};
    assert(address_space_clone_cow(sched_task_as(shared),&child));
    flags=address_space_user_page_flags(&child,address);
    assert((flags&RELIEFNT_PAGE_WRITE_NOTIFY) && !(flags&(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_COW)));
    address_space_destroy(&child); memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    flushes=fixture_tlb_flushes;
    assert(!syscall_mm_mprotect(address,4096,LINUX_PROT_READ));
    assert(fixture_tlb_flushes>flushes);
    assert(!syscall_handle_user_page_fault_detailed(address,7,true));
    memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    assert(!syscall_mm_mprotect(address,4096,LINUX_PROT_READ|LINUX_PROT_WRITE));
    memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    struct task *private=file_task(source,node,address,0,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ|LINUX_PROT_WRITE);
    prepare_task(private);
    assert(syscall_handle_task_page_fault(private,address,4));
    uint64_t original=address_space_user_page_phys(sched_task_as(shared),address);
    assert(address_space_user_page_phys(sched_task_as(private),address)==original);
    assert(syscall_handle_user_page_fault_detailed(address,7,true)==1);
    uint64_t copy=address_space_user_page_phys(sched_task_as(private),address);
    assert(copy && copy!=original); *(uint8_t *)mm_phys_to_virt(copy)=0x72;
    assert(*(uint8_t *)mm_phys_to_virt(original)==0x31);
    memory_snapshot_get(&snapshot); assert(!snapshot.dirty_kib);
    kernel_execution_unlock_irqrestore(lock); destroy_task(private); destroy_task(shared);
    page_cache_forget_node(&node); vfs_exec_source_put(source); assert(!live_pages);
    puts("PASS permission grants, forked PTEs and private COW leave file cache clean");
}

/** @brief Make one real shared-cache notification PTE for modeled faults. */
static struct task *stale_shared_task(struct storage_node *node, struct vfs_exec_source **source,
                                      uint64_t address)
{
    *node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
        .first_cluster=940,.volume_id=3,.size=4096};
    memset(file_bytes,0x31,4096); *source=exec_fixture_open(node);
    struct task *task=file_task(*source,*node,address,0,node->size,
        TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ|LINUX_PROT_WRITE);
    prepare_task(task); assert(syscall_handle_task_page_fault(task,address,4));
    uint64_t flags=address_space_user_page_flags(sched_task_as(task),address);
    assert((flags&RELIEFNT_PAGE_WRITE_NOTIFY) && !(flags&RELIEFNT_PAGE_WRITABLE));
    return task;
}

/** @brief Two admitted hardware reports must each flush their own stale TLB. */
static void stale_shared(void)
{
    const uint64_t address=0x30000000; struct storage_node node; struct vfs_exec_source *source;
    uint64_t lock; kernel_execution_lock_irqsave(&lock);
    struct task *task=stale_shared_task(&node,&source,address);
    uint64_t frame=address_space_user_page_phys(sched_task_as(task),address), before=fixture_invlpg_count;
    assert(syscall_handle_user_page_fault_detailed(address,7,true)==1);
    assert(fixture_invlpg_count==before+1 && fixture_invlpg_last==address);
    uint64_t flags=address_space_user_page_flags(sched_task_as(task),address);
    assert((flags&RELIEFNT_PAGE_WRITABLE) && !(flags&RELIEFNT_PAGE_WRITE_NOTIFY));
    struct page_cache_report old, now; page_cache_report(&old); assert(old.dirty==1);
    uint64_t pages=live_pages; unsigned reads=atomic_load(&provider_reads);
    before=fixture_invlpg_count;
    int stale=syscall_handle_user_page_fault_detailed(address,7,true);
    if (stale!=1) fprintf(stderr,"second shared stale fault returned %d; writable=%d\n",stale,!!(flags&RELIEFNT_PAGE_WRITABLE));
    assert(stale==1 && fixture_invlpg_count==before+1 && fixture_invlpg_last==address);
    assert(address_space_user_page_flags(sched_task_as(task),address)==flags);
    assert(live_pages==pages && atomic_load(&provider_reads)==reads);
    page_cache_report(&now); assert(now.dirty==old.dirty);
    *(uint8_t *)mm_phys_to_virt(frame)=0x72;
    assert(*(uint8_t *)mm_phys_to_virt(frame)==0x72);
    kernel_execution_unlock_irqrestore(lock); destroy_task(task);page_cache_forget_node(&node);
    vfs_exec_source_put(source);assert(!live_pages);
    puts("PASS shared first write and stale user protection fault each invalidate locally");
}

/** @brief Model the lock-held FUTEX_WAKE_OP supervisor trap and stale retry. */
static void stale_supervisor(void)
{
    const uint64_t address=0x30000000; struct storage_node node; struct vfs_exec_source *source;
    uint64_t outer, flags; kernel_execution_lock_irqsave(&outer);
    struct task *task=stale_shared_task(&node,&source,address);
    assert(user_range_writable(address,4)); /* Actual preflight, before futex_lock. */
    uint64_t frame=address_space_user_page_phys(sched_task_as(task),address);
    struct fault_work work={task,address,3,0x08,0,NULL,0};
    uint64_t page_count=live_pages; unsigned reads=atomic_load(&provider_reads), schedules_before=atomic_load(&schedules);
    kernel_spin_lock_irqsave(&futex_lock,&flags);
    for (unsigned i=0;i<2;++i) {
        uint64_t old=fixture_invlpg_count; forbidden_fault_calls=0; measure_fault_context=true;
        run_fault(&work); measure_fault_context=false;
        assert(!work.result && work.owner==2 && futex_lock.state && !user_page_fault_lock.state);
        assert(writer_owned() && fixture_invlpg_count==old+1 && fixture_invlpg_last==address);
        assert(!forbidden_fault_calls && live_pages==page_count && atomic_load(&provider_reads)==reads &&
               atomic_load(&schedules)==schedules_before);
    }
    kernel_spin_unlock_irqrestore(&futex_lock,flags);
    assert(address_space_user_page_flags(sched_task_as(task),address)&RELIEFNT_PAGE_WRITABLE);
    struct page_cache_report cache; page_cache_report(&cache); assert(cache.dirty==1);
    *(uint32_t *)mm_phys_to_virt(frame)=0x12345678;
    assert(*(uint32_t *)mm_phys_to_virt(frame)==0x12345678);
    kernel_execution_unlock_irqrestore(outer); destroy_task(task);page_cache_forget_node(&node);
    vfs_exec_source_put(source);assert(!live_pages);
    puts("PASS supervisor first write and stale retry under real futex lock without allocation, I/O or scheduling");
}

/** @brief Genuine denied or non-cache leaves never take the stale-write retry. */
static void stale_negative(void)
{
    const uint64_t address=0x30000000; struct storage_node node;struct vfs_exec_source *source;
    uint64_t lock;kernel_execution_lock_irqsave(&lock);
    struct task *task=stale_shared_task(&node,&source,address); struct address_space *as=sched_task_as(task);
    assert(syscall_handle_user_page_fault_detailed(address,7,true)==1);
    uint64_t *pte=address_space_pte(as,address); assert(pte); uint64_t old=*pte, flushes=fixture_invlpg_count;
    uint32_t prot=task->vmas[0].prot, flags=task->vmas[0].flags, node_flags=task->vmas[0].file_node.flags;
#define DENIED(addr,error) do { assert(!syscall_handle_user_page_fault_detailed((addr),(error),true)); \
    assert(!syscall_handle_user_page_fault_detailed((addr),(error)&~0x4ULL,false)); \
    assert(fixture_invlpg_count==flushes); } while (0)
    task->vmas[0].prot=LINUX_PROT_READ; DENIED(address,7);
    task->vmas[0].prot=LINUX_PROT_NONE; DENIED(address,7); task->vmas[0].prot=prot;
    DENIED(address,15); DENIED(address,23); DENIED(address,39); DENIED(address,71);
    DENIED(RELIEFNT_USER_TOP,7); DENIED(address+4096,7);
    task->vmas[0].flags=(flags&~TASK_VMA_FLAG_SHARED)|TASK_VMA_FLAG_PRIVATE; DENIED(address,7);
    task->vmas[0].flags=flags|TASK_VMA_FLAG_DEVICE; DENIED(address,7);
    task->vmas[0].flags=flags&~TASK_VMA_FLAG_SHARED_FILE; DENIED(address,7);
    task->vmas[0].flags=flags;task->vmas[0].file_node.flags=node_flags|STORAGE_NODE_FLAG_TMPFS; DENIED(address,7);
    task->vmas[0].file_node.flags=node_flags;
    *pte=old|RELIEFNT_PAGE_DEVICE; DENIED(address,7);
    *pte=old&~RELIEFNT_PAGE_WRITABLE; DENIED(address,7);
    *pte=(old&~RELIEFNT_PAGE_PRESENT)|RELIEFNT_PAGE_PROTNONE; DENIED(address,7);
    *pte=RELIEFNT_PAGE_USER|RELIEFNT_PAGE_SWAP; DENIED(address,7);
    *pte=0; assert(!address_space_shared_write_fault(as,address)); assert(fixture_invlpg_count==flushes);
    uint64_t foreign=mm_alloc_page();assert(foreign);
    *pte=foreign|RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_USER|RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITABLE;
    DENIED(address,7); *pte=old; mm_free_page(foreign);
    uint64_t pml4=as->pml4[(address>>39)&511], *pdpt=mm_phys_to_virt(pml4&RELIEFNT_PHYS_ADDR_MASK);
    uint64_t *pd=mm_phys_to_virt(pdpt[(address>>30)&511]&RELIEFNT_PHYS_ADDR_MASK);
    uint64_t *pde=&pd[(address>>21)&511], pde_old=*pde; *pde &= ~RELIEFNT_PAGE_WRITABLE;
    DENIED(address,7); *pde=pde_old;
#undef DENIED
    kernel_execution_unlock_irqrestore(lock);destroy_task(task);page_cache_forget_node(&node);
    vfs_exec_source_put(source);assert(!live_pages);
    puts("PASS denied, reserved, fetch, private, device, non-cache, swap, missing and upper-level protection faults stay denied");
}

int main(int argc, char **argv)
{
    assert(argc == 3); nx_enabled = true; page_cache_init();
    uintptr_t target = (uintptr_t)strtoull(argv[2], NULL, 16); assert(target);
    if (!strcmp(argv[1], "cache-prefix")) cache_prefix();
    else if (!strcmp(argv[1], "cache-interleaved")) cache_interleaved();
    else if (!strcmp(argv[1], "budget-cold")) dispatch_budget(false, target);
    else if (!strcmp(argv[1], "budget-refresh")) dispatch_budget(true, target);
    else if (!strcmp(argv[1], "budget-full")) dispatch_full_capacity();
    else if (!strcmp(argv[1], "retry-log")) retry_log();
    else if (!strcmp(argv[1], "backing-log")) backing_log();
    else if (!strcmp(argv[1], "actual-events")) actual_events();
    else if (!strcmp(argv[1], "dirty-cache")) dirty_cache();
    else if (!strcmp(argv[1], "dirty-policy")) dirty_policy();
    else if (!strcmp(argv[1], "prefault-shortage")) prefault_shortage(false);
    else if (!strcmp(argv[1], "usercopy-shortage")) prefault_shortage(true);
    else if (!strcmp(argv[1], "ring0-log")) ring0_log();
    else if (!strcmp(argv[1], "prefault-terminal")) prefault_terminal();
    else if (!strcmp(argv[1], "stale-shared")) stale_shared();
    else if (!strcmp(argv[1], "stale-supervisor")) stale_supervisor();
    else if (!strcmp(argv[1], "stale-negative")) stale_negative();
    else abort();
    return 0;
}

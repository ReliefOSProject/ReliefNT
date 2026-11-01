/* Real trap, execution gate, pressure engine, queue, scheduler sleep and
 * swap PTE/slot; physical-page availability, disk and timer wake are modeled. */
#define _POSIX_C_SOURCE 200809L
#include "../../kernel/sched/internal.h"
#define PRESSURE_REAL_FIXTURE 1
#define PRESSURE_FAULT_TEST 1
#define SWAP_FAULT_GATE_FIXTURE_ENTRY old_fault_gate_main
#include "swap_fault_gate_test.c"

static unsigned pressure_baseline_pages;
static void (*retirement_notify)(void);
#ifdef PRESSURE_FIX5_FIXTURE
static bool pressure_fixture_low_jitter;
static unsigned pressure_fixture_free_reads;
#endif
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
uint64_t mm_free_memory_kib(void)
{
    uint64_t pages = 64 + (pressure_baseline_pages > live_pages ? pressure_baseline_pages - live_pages : 0);
#ifdef PRESSURE_FIX5_FIXTURE
    if (pressure_fixture_low_jitter) pages += pressure_fixture_free_reads++ & 1u;
#endif
    return 4 * pages;
}
#ifdef PRESSURE_OPEN_FIXTURE
static bool pressure_fixture_storage_busy;
bool swap_storage_busy(void) { return pressure_fixture_storage_busy; }
#else
bool swap_storage_busy(void) { return false; }
#endif
#ifdef PRESSURE_FIX5_FIXTURE
static int pressure_fixture_no_victim(uint32_t budget);
int memory_oom_advance(uint32_t budget) { return pressure_fixture_no_victim(budget); }
#else
int memory_oom_advance(uint32_t budget) { (void)budget; abort(); }
#endif
void sched_set_memory_release_notify(void (*notify)(void)) { retirement_notify=notify; }

#ifndef PRESSURE_INTEGRATION_FIXTURE_ENTRY
#define PRESSURE_INTEGRATION_FIXTURE_ENTRY main
#endif
int PRESSURE_INTEGRATION_FIXTURE_ENTRY(void)
{
    nx_enabled=true; page_cache_init();
    uint8_t header[4096]={0}; header[1024]=1; header[1028]=3;
    memcpy(header+4086,"SWAPSPACE2",10);
    struct swap_area *area=NULL;
    assert(!swap_area_create(header,sizeof(header),4,1,&area));
    assert(!swap_area_publish(area));
    struct task *task=new_task();
    const uint64_t target=RELIEFNT_USER_BASE+0x50000, target2=target+4096;
    const uint64_t victim=target+2*4096, victim2=target+3*4096;
    task->vmas[0]=(struct task_vma){.used=1,.start=target,.end=victim2+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t a=mm_alloc_page(), a2=mm_alloc_page(), b=mm_alloc_page(), b2=mm_alloc_page();
    assert(a && a2 && b && b2);
    memset(mm_phys_to_virt(a),0x69,4096); memset(mm_phys_to_virt(a2),0x79,4096);
    memset(mm_phys_to_virt(b),0x97,4096); memset(mm_phys_to_virt(b2),0xa7,4096);
    assert(address_space_map_user_page(sched_task_as(task),target,a,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(sched_task_as(task),target2,a2,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(sched_task_as(task),victim,b,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(sched_task_as(task),victim2,b2,RELIEFNT_PAGE_WRITABLE));
    cpu=0; current=task; current_task[0]=task; current_pid[0]=task->pid;
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    assert(swap_try_evict(sched_task_as(task),target)==1);
    assert(swap_try_evict(sched_task_as(task),target2)==1);
    kernel_execution_unlock_irqrestore(flags);
    pressure_baseline_pages=live_pages;
    atomic_store(&fail_alloc,true);
    struct fault_work work={task,target,6,0x23,0,NULL,0};
    unsigned attempts=0, blocked=0;
    while (live_pages == pressure_baseline_pages) {
        run_fault(&work); assert(!atomic_load(&signals));
        if (task->state==TASK_BLOCKED) { assert(task->waiting_queue); ++blocked; }
        assert(++attempts < 1000);
    }
    assert(blocked && retirement_notify &&
           paging_user_entry_kind(sched_task_as(task),victim)==PAGING_USER_ENTRY_SWAP);
    atomic_store(&fail_alloc,false);
    run_fault(&work);
    a=address_space_user_page_phys(sched_task_as(task),target); assert(a && !task->waiting_queue);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(a))[i]==0x69);
    pressure_baseline_pages=live_pages; work.address=target2; atomic_store(&fail_alloc,true);
    unsigned second_attempts=0;
    while (live_pages == pressure_baseline_pages) {
        run_fault(&work); assert(!atomic_load(&signals) && ++second_attempts < 5);
    }
    assert(paging_user_entry_kind(sched_task_as(task),victim2)==PAGING_USER_ENTRY_SWAP);
    atomic_store(&fail_alloc,false); run_fault(&work);
    a2=address_space_user_page_phys(sched_task_as(task),target2); assert(a2 && !task->waiting_queue);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(a2))[i]==0x79);
    kernel_execution_lock_irqsave(&flags);
    assert(swap_resolve_fault(task,victim,4)==1);
    assert(swap_resolve_fault(task,victim2,4)==1);
    kernel_execution_unlock_irqrestore(flags);
    b=address_space_user_page_phys(sched_task_as(task),victim); assert(b);
    b2=address_space_user_page_phys(sched_task_as(task),victim2); assert(b2);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(b))[i]==0x97);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(b2))[i]==0xa7);
    destroy_task(task); assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
    printf("PASS actual trap-to-pressure-to-queue self-MM eviction/page-in first=%u second=%u blocked=%u bytes=16384\n",
           attempts,second_attempts,blocked);
    return 0;
}

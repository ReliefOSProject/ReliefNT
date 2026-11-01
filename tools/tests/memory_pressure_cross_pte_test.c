/* Real pressure traversal, scheduler retry history, PTEs and swap slots.
 * Host RAM, CPU ownership, clean-cache scan and disk transport are modeled. */
#include "../../kernel/sched/internal.h"
#define SWAP_PAGING_FIXTURE_ENTRY old_swap_paging_main
#define current_task fixture_current_task
#include "swap_paging_test.c"
#undef current_task
#include <reliefnt/memory_pressure.h>

static unsigned quota;
static uint64_t cache_epoch;
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
uint64_t mm_free_memory_kib(void)
{ return 4 * (64 + (quota > allocated ? quota - allocated : 0)); }
uint32_t sched_current_pid(void) { return fixture_current_task ? fixture_current_task->pid : 0; }
bool swap_storage_busy(void) { return false; }
uint32_t page_cache_pressure_scan(uint32_t budget, uint32_t *inspected, bool *cycle)
{ assert(budget); *inspected=1; *cycle=true; ++cache_epoch; return 0; }
uint32_t page_cache_pressure_scan_count(uint32_t budget, uint32_t *inspected, bool *cycle, uint32_t *candidates)
{ *candidates=0; return page_cache_pressure_scan(budget,inspected,cycle); }
uint64_t page_cache_pressure_epoch(void) { return cache_epoch; }
int memory_oom_advance(uint32_t budget) { (void)budget; abort(); }
void memory_pressure_notify(void) { }

static void reclaim_one(struct task *task, uint64_t rip, uint64_t fault)
{
    memory_reclaim_fault_scope_begin(task, rip, fault);
    int rc; unsigned batches=0;
    do { rc=memory_try_reclaim(1,0); assert(++batches<5000); } while (rc==-EAGAIN);
    memory_reclaim_fault_scope_end();
    assert(!rc && batches);
}

static void verify_page(struct task *task, uint64_t page, uint8_t value)
{
    uint64_t phys=address_space_user_page_phys(sched_task_as(task),page); assert(phys);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(phys))[i]==value);
}

int main(void)
{
    uint8_t header[4096]={0}; header[1024]=1; header[1028]=3;
    memcpy(header+4086,"SWAPSPACE2",10);
    struct swap_area *area=NULL;
    assert(!swap_area_create(header,sizeof(header),4,1,&area));
    assert(!swap_area_publish(area));
    struct sched_task_object *object=calloc(1,sizeof(*object)); assert(object);
    struct task *task=&object->task;
    task->pid=123; task->kind=TASK_KIND_USER; task->state=TASK_RUNNING;
    fixture_current_task=task; nx_enabled=true;
    const uint64_t base=RELIEFNT_USER_BASE+0x100000, rip=base+32;
    task->vmas[0]=(struct task_vma){.used=1,.start=base,.end=base+8*4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(task)));
    for (unsigned i=0;i<8;++i) {
        uint64_t frame=mm_alloc_page(); assert(frame); memset(mm_phys_to_virt(frame),0x31+i,4096);
        assert(address_space_map_user_page(sched_task_as(task),base+i*4096,frame,
                                           RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    }
    /* Model two source/destination page faults of one multi-operand instruction. */
    assert(swap_try_evict(sched_task_as(task),base+3*4096)==1);
    assert(swap_try_evict(sched_task_as(task),base+4*4096)==1);
    uint64_t held1=mm_alloc_page(), held2=mm_alloc_page(); assert(held1 && held2);
    quota=allocated;
    sched_pressure_fault_resolved(task,rip,base+4096);
    sched_pressure_fault_resolved(task,rip,base+2*4096);
    reclaim_one(task,rip,base+3*4096);
    assert(paging_user_entry_kind(sched_task_as(task),base+5*4096)==PAGING_USER_ENTRY_SWAP);
    assert(swap_resolve_fault(task,base+3*4096,4)==1);
    sched_pressure_fault_resolved(task,rip,base+3*4096);
    for (unsigned i=0;i<4;++i) verify_page(task,base+i*4096,0x31+i);
    reclaim_one(task,rip,base+4*4096);
    assert(paging_user_entry_kind(sched_task_as(task),base+6*4096)==PAGING_USER_ENTRY_SWAP);
    assert(swap_resolve_fault(task,base+4*4096,4)==1);
    sched_pressure_fault_resolved(task,rip,base+4*4096);
    for (unsigned i=0;i<5;++i) verify_page(task,base+i*4096,0x31+i);
    mm_free_page(held1); mm_free_page(held2);
    assert(swap_resolve_fault(task,base+5*4096,4)==1);
    assert(swap_resolve_fault(task,base+6*4096,4)==1);
    for (unsigned i=0;i<8;++i) verify_page(task,base+i*4096,0x31+i);
    /* Same RIP over a larger loop slides its bounded history: page1 is eligible. */
    for (unsigned i=4;i<8;++i) sched_pressure_fault_resolved(task,rip,base+i*4096);
    quota=allocated;
    reclaim_one(task,rip,base+8*4096);
    assert(paging_user_entry_kind(sched_task_as(task),base+4096)==PAGING_USER_ENTRY_SWAP);
    assert(swap_resolve_fault(task,base+4096,4)==1);
    for (unsigned i=0;i<8;++i) verify_page(task,base+i*4096,0x31+i);
    address_space_destroy(sched_task_as(task)); fixture_current_task=NULL; free(object);
    assert(!allocated && !swap_area_unpublish(area)); swap_area_destroy(area);
    puts("PASS two same-RIP multi-operand page-ins and sliding history with real PTE/slot bytes");
    return 0;
}

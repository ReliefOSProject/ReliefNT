/* Real trap, execution locks, swapped PTE and slot; the pressure wait hook is
 * modeled here and its queue/scheduler implementation has a separate test. */
#define SWAP_FAULT_GATE_FIXTURE_ENTRY old_swap_fault_gate_main
#define PRESSURE_FAULT_TEST 1
#include "swap_fault_gate_test.c"

static unsigned pressure_fault_calls;
uint64_t mm_free_memory_kib(void) { return atomic_load(&fail_alloc) ? 256 : 4096; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
int memory_fault_oom(struct task *task, uint64_t address)
{
    assert(task == current && address == fault_address && writer_owned() &&
           execution_depth[cpu] == 1 && !reader_owned() && !user_page_fault_lock.state);
    ++pressure_fault_calls;
    sched_sleep_current_until(1);
    return -EAGAIN;
}

int main(void)
{
    nx_enabled=true; page_cache_init();
    uint8_t header[4096]={0}; header[1024]=1; header[1028]=3;
    memcpy(header+4086,"SWAPSPACE2",10);
    struct swap_area *area=NULL;
    assert(!swap_area_create(header,sizeof(header),4,1,&area));
    assert(!swap_area_publish(area));
    struct task *task=new_task();
    const uint64_t va=RELIEFNT_USER_BASE+0x50000;
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t frame=mm_alloc_page(); assert(frame); memset(mm_phys_to_virt(frame),0xa5,4096);
    assert(address_space_map_user_page(sched_task_as(task),va,frame,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    cpu=0; current=task; current_task[0]=task; current_pid[0]=task->pid;
    uint64_t irq; kernel_execution_lock_irqsave(&irq);
    assert(swap_try_evict(sched_task_as(task),va)==1);
    kernel_execution_unlock_irqrestore(irq);
    struct fault_work work={task,va,6,0x23,0,NULL,0};
    atomic_store(&fail_alloc,true); run_fault(&work); atomic_store(&fail_alloc,false);
    printf("low-memory swap fault: signals=%u waits=%u pressure=%u\n",
           atomic_load(&signals),gate_sleeps,pressure_fault_calls); fflush(stdout);
    assert(!atomic_load(&signals) && gate_sleeps==1 && pressure_fault_calls==1);
    assert(paging_user_entry_kind(sched_task_as(task),va)==PAGING_USER_ENTRY_SWAP);
    run_fault(&work);
    frame=address_space_user_page_phys(sched_task_as(task),va); assert(frame);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i]==0xa5);
    assert(!atomic_load(&signals) && !writer_owned() && !user_page_fault_lock.state);
    const uint64_t anon = va + 4096;
    task->vmas[1]=(struct task_vma){.used=1,.start=anon,.end=anon+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    work.address=anon; gate_sleeps=0; pressure_fault_calls=0;
    atomic_store(&fail_alloc,true); run_fault(&work); atomic_store(&fail_alloc,false);
    printf("low-memory anonymous fault: signals=%u waits=%u pressure=%u\n",
           atomic_load(&signals),gate_sleeps,pressure_fault_calls); fflush(stdout);
    assert(!atomic_load(&signals) && gate_sleeps==1 && pressure_fault_calls==1);
    run_fault(&work);
    frame=address_space_user_page_phys(sched_task_as(task),anon); assert(frame);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i]==0);
    assert(!atomic_load(&signals) && !writer_owned() && !user_page_fault_lock.state);
    const uint64_t shared = anon + 4096;
    task->vmas[2]=(struct task_vma){.used=1,.start=shared,.end=shared+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    task->shared_mm=&task->address_space;
    work.address=shared; gate_sleeps=0; pressure_fault_calls=0;
    atomic_store(&fail_alloc,true); run_fault(&work); atomic_store(&fail_alloc,false);
    printf("low-memory shared-MM fault: signals=%u waits=%u pressure=%u\n",
           atomic_load(&signals),gate_sleeps,pressure_fault_calls); fflush(stdout);
    assert(!atomic_load(&signals) && gate_sleeps==1 && pressure_fault_calls==1);
    run_fault(&work);
    frame=address_space_user_page_phys(sched_task_as(task),shared); assert(frame);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i]==0);
    task->shared_mm=NULL;
    struct task *child=calloc(1,sizeof(*child)); assert(child);
    child->kind=TASK_KIND_USER; child->vmas[0]=task->vmas[0];
    assert(address_space_clone_cow(sched_task_as(task),sched_task_as(child)));
    uint64_t parent_frame=address_space_user_page_phys(sched_task_as(task),va);
    struct fault_work cow={child,va,7,0x23,0,NULL,0};
    gate_sleeps=0; pressure_fault_calls=0;
    atomic_store(&fail_alloc,true); run_fault(&cow); atomic_store(&fail_alloc,false);
    printf("low-memory COW fault: signals=%u waits=%u pressure=%u\n",
           atomic_load(&signals),gate_sleeps,pressure_fault_calls); fflush(stdout);
    assert(!atomic_load(&signals) && gate_sleeps==1 && pressure_fault_calls==1);
    run_fault(&cow);
    frame=address_space_user_page_phys(sched_task_as(child),va);
    assert(frame && frame!=parent_frame);
    for (unsigned i=0;i<4096;++i) {
        assert(((uint8_t *)mm_phys_to_virt(frame))[i]==0xa5);
        assert(((uint8_t *)mm_phys_to_virt(parent_frame))[i]==0xa5);
    }
    destroy_task(child);
    destroy_task(task); assert(!live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
    puts("PASS allocation-short swap/anonymous faults wait and retry their full-page contents");
    return 0;
}

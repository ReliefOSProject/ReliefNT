/* SPDX-License-Identifier: Apache-2.0 */
/* Real ring-3 fault dispatcher, private mapping admission, scheduler wait,
 * execution gate, reclaim engine, page tables, and swap slots. The finite
 * ordinary pool, RAM swap transport and timer wakes are explicit host models. */
#define GATE_MEDIA_PAGES 128
#define mm_total_memory_kib pressure_fixture_original_total_kib
#define PRESSURE_FIX5_FIXTURE_ENTRY pressure_fix5_unused_main
#include "memory_pressure_fix5_test.c"
#undef mm_total_memory_kib
#include "../../mm/reclaim/internal.h"

static bool churn_names;
static unsigned name_evictions;
uint32_t __real_vfs_lookup_cache_reclaim(uint32_t budget);
uint32_t __wrap_vfs_lookup_cache_reclaim(uint32_t budget)
{
    if(!churn_names)return __real_vfs_lookup_cache_reclaim(budget);
    /* Model an indefinitely replenished name cache whose objects share slab
     * pages with live allocations. Eviction frees no physical frame. */
    assert(budget<=8);name_evictions+=budget;return budget;
}

static unsigned probe_allocations;
uint64_t __real_mm_alloc_page(void);
uint64_t __wrap_mm_alloc_page(void)
{
    assert(writer_owned() && !reader_owned() && !user_page_fault_lock.state);
    ++probe_allocations;return __real_mm_alloc_page();
}

static bool inject_write_eio, inject_write_contended;
static unsigned injected_writes;
int __real_swap_backend_page_io(struct swap_area *,uint32_t,uint64_t,bool);
int __wrap_swap_backend_page_io(struct swap_area *area,uint32_t page,uint64_t phys,bool write)
{
    if(write && (inject_write_eio || inject_write_contended)) {
        assert(writer_owned() && !reader_owned() && !user_page_fault_lock.state);
        ++injected_writes;
        return inject_write_eio ? -EIO : SWAP_STORAGE_CONTENDED;
    }
    return __real_swap_backend_page_io(area,page,phys,write);
}

/* 256 pages => existing watermark formula yields low=64, high=96 ordinary. */
uint64_t mm_total_memory_kib(void) { return 256 * 4; }

/* Exec commit is real; only its privileged CR3 load and unrelated owners are
 * modeled. These fixtures have no vfork relation or external VMA leases. */
void __wrap_paging_load_cr3(uint64_t cr3) { assert(writer_owned() && cr3==paging_kernel_cr3()); }
void sched_vfork_child_done(struct task *task) { assert(writer_owned() && !task->vfork_parent); }
void graphics_service_cancel_task_continuation(struct task *task) { assert(writer_owned() && task); }
void sched_task_vma_release(struct task *task)
{
    assert(writer_owned() && task);
    for(unsigned i=0;i<SCHED_TASK_VMA_MAX;++i)
        assert(!task->vmas[i].mapping_file && !task->vmas[i].device_mapping &&
               !task->vmas[i].exec_source && !task->vmas[i].inode);
}

static void early_refill(void)
{
    struct swap_area *area=setup_area();
    struct task *task=new_task();
    const uint64_t target=0x20000000, victims=0x30000000;
    task->vmas[0]=(struct task_vma){.used=1,.start=target,.end=target+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(task),target,target+8192));
    create_victims(task,victims,48);
    task->vmas[1].end=victims+96*4096;
    for(unsigned i=48;i<96;++i) {
        uint64_t phys=mm_alloc_page();assert(phys);memset(mm_phys_to_virt(phys),0x40+i,4096);
        assert(address_space_map_user_page(sched_task_as(task),victims+i*4096,phys,
                                          RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    }
    prepare_task(task);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    assert(mm_free_memory_kib()/4==64+63);
    struct fault_work work={task,target,6,0x23,0,NULL,0};
    unsigned before_pages=live_pages;run_fault(&work);
    assert(probe_allocations==1 && live_pages==before_pages);
    if(address_space_user_page_phys(sched_task_as(task),target))
        fprintf(stderr,"eligible demand fault consumed a frame below low instead of starting refill\n");
    assert(!address_space_user_page_phys(sched_task_as(task),target));
    assert(task->state==TASK_BLOCKED && task->waiting_queue && !gate_writes && !oom_calls &&
           mm_free_memory_kib()/4>mm_emergency_reserve_pages());
    unsigned attempts=1;
    while(!address_space_user_page_phys(sched_task_as(task),target)) {
        unsigned before=gate_writes;run_fault(&work);
        assert(gate_writes-before<=16 && !atomic_load(&signals) && ++attempts<1800);
    }
    assert(mm_free_memory_kib()/4>=64+95 && gate_writes>=33 && !oom_calls && !task->waiting_queue &&
           probe_allocations==1);
    unsigned first_attempts=attempts,writes=gate_writes;
    pressure_baseline_pages=live_pages+63;work.address=target+4096;attempts=0;
    do {
        unsigned before=gate_writes;run_fault(&work);
        assert(gate_writes-before<=16 && !atomic_load(&signals) && ++attempts<200);
    } while(!address_space_user_page_phys(sched_task_as(task),work.address));
    assert(mm_free_memory_kib()/4>=64+95 && gate_writes-writes>=33 && !oom_calls &&
           !task->waiting_queue && probe_allocations==2);
    restore_victims(task,victims,96);quota_enforced=false;destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area));swap_area_destroy(area);
    printf("PASS low→high refills preserve cursor: first=%u subsequent=%u entries\n",first_attempts,attempts);
}
struct headroom_case {
    struct swap_area *area;
    struct task *task;
    uint64_t target,victims;
    unsigned count;
};

static struct headroom_case make_case_no_generation(unsigned count)
{
    struct headroom_case c={.area=setup_area(),.task=new_task(),.target=0x20000000,
                            .victims=0x30000000,.count=count};
    c.task->vmas[0]=(struct task_vma){.used=1,.start=c.target,.end=c.target+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(c.task),c.target,c.target+8192));
    if(count)create_victims(c.task,c.victims,count);
    assert(!mm_context_generation_existing(sched_task_as(c.task)));return c;
}
static struct headroom_case make_case(unsigned count)
{
    struct headroom_case c=make_case_no_generation(count);
    prepare_task(c.task);return c;
}
static void finish_case(struct headroom_case *c)
{
    quota_enforced=false;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    for(unsigned i=0;i<2;++i) {
        uint64_t page=c->target+i*4096;
        if(paging_user_entry_kind(sched_task_as(c->task),page)==PAGING_USER_ENTRY_SWAP)
            assert(swap_resolve_fault(c->task,page,4)==1);
    }
    memory_headroom_cancel(c->task);
    kernel_execution_unlock_irqrestore(flags);
    if(c->count)restore_victims(c->task,c->victims,c->count);
    destroy_task(c->task);assert(!live_pages && !swap_area_unpublish(c->area));swap_area_destroy(c->area);
}
static void above_low(void)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+65;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    assert(address_space_user_page_phys(sched_task_as(c.task),c.target) &&
           !c.task->waiting_queue && !gate_writes && !oom_calls && !atomic_load(&signals) &&
           !probe_allocations);
    finish_case(&c);puts("PASS above low uses normal fast fault path");
}

static void partial_fallback(void)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned attempts=0;
    do { unsigned before=gate_writes;run_fault(&work);assert(gate_writes-before<=16);
         assert(!atomic_load(&signals) && ++attempts<1800);
    } while(!address_space_user_page_phys(sched_task_as(c.task),c.target));
    assert(gate_writes==4 && !oom_calls && mm_free_memory_kib()/4<64+96);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(!state->active && state->cooldown && !c.task->waiting_queue);
    unsigned writes=gate_writes;work.address+=4096;run_fault(&work);
    assert(address_space_user_page_phys(sched_task_as(c.task),work.address) &&
           gate_writes==writes && !c.task->waiting_queue && state->cooldown && !oom_calls);
    finish_case(&c);printf("PASS partial no-victim fallback and unchanged-state cooldown (%u entries)\n",attempts);
}

/* An independently replenished pool clears cooldown at HIGH, without a
 * speculative scan or consuming the still-protected emergency pages. */
static void high_recovery(void)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned attempts=0;
    while(!address_space_user_page_phys(sched_task_as(c.task),c.target)) {
        run_fault(&work);assert(!atomic_load(&signals) && ++attempts<1800);
    }
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->cooldown && !state->active);
    pressure_baseline_pages=live_pages+96;
    unsigned writes=gate_writes;work.address+=4096;run_fault(&work);
    assert(address_space_user_page_phys(sched_task_as(c.task),work.address) &&
           !state->active && !state->cooldown && !c.task->waiting_queue &&
           gate_writes==writes && !oom_calls && mm_free_memory_kib()/4==64+95);
    finish_case(&c);puts("PASS high-water recovery clears optional cooldown");
}

static void swap_activation(void)
{
    struct headroom_case c=make_case(48);
    assert(!swap_area_unpublish(c.area));
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned attempts=0;
    do {run_fault(&work);assert(!atomic_load(&signals) && ++attempts<1800);}
    while(!address_space_user_page_phys(sched_task_as(c.task),c.target));
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->cooldown && !state->active && !gate_writes && !oom_calls);
    assert(!swap_area_publish(c.area));work.address+=4096;run_fault(&work);
    assert(!address_space_user_page_phys(sched_task_as(c.task),work.address) &&
           state->active && c.task->state==TASK_BLOCKED);
    attempts=1;
    while(!address_space_user_page_phys(sched_task_as(c.task),work.address)) {
        unsigned before=gate_writes;run_fault(&work);
        assert(gate_writes-before<=16 && !atomic_load(&signals) && ++attempts<1800);
    }
    assert(!state->active && !state->cooldown && gate_writes && !oom_calls &&
           mm_free_memory_kib()/4>=64+95);
    finish_case(&c);puts("PASS no-swap cooldown reopens after low-memory swapon");
}

static void protection(bool cancel_active)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};
    if(cancel_active) {
        run_fault(&work);assert(c.task->waiting_queue && c.task->state==TASK_BLOCKED);
        assert(sched_pressure_headroom_state(c.task)->active);
    }
    c.task->vmas[0].prot=LINUX_PROT_READ;run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(atomic_load(&signals)==1 && !gate_writes && !oom_calls && !c.task->waiting_queue &&
           !state->active && !state->cooldown &&
           !address_space_user_page_phys(sched_task_as(c.task),c.target));
    finish_case(&c);
    printf("PASS %s real protection failure does not refill or choose OOM\n",
           cancel_active ? "cancelled" : "initial");
}

/* An optional write error/foreign storage owner cannot turn the fault into
 * OOM or lose the victim; the latter has the existing finite stall bound. */
static void storage_fallback(bool contended)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    inject_write_contended=contended;inject_write_eio=!contended;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned attempts=0;
    while(!address_space_user_page_phys(sched_task_as(c.task),c.target)) {
        unsigned before=injected_writes;run_fault(&work);
        assert(injected_writes-before<=16 && !atomic_load(&signals) && ++attempts<1800);
    }
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->cooldown && !state->active && !c.task->waiting_queue &&
           !gate_writes && injected_writes && !oom_calls &&
           paging_user_entry_kind(sched_task_as(c.task),c.victims)==PAGING_USER_ENTRY_RESIDENT &&
           !mm_swap_record_lookup(sched_task_as(c.task),c.victims));
    inject_write_eio=inject_write_contended=false;finish_case(&c);
    printf("PASS optional %s falls back with victim and slot ownership intact (%u entries)\n",
           contended ? "storage contention" : "write EIO",attempts);
}

/* A changed CR3 cancels the old scalar cursor before considering the new MM. */
static void root_change(void)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->active && c.task->waiting_queue);
    uint64_t old_cr3=state->cr3;
    struct address_space old=c.task->address_space.as,fresh={0};
    quota_enforced=false;assert(address_space_create(&fresh));
    assert(address_space_prepare_user_range(&fresh,c.target,c.target+8192));
    c.task->address_space.as=fresh;pressure_baseline_pages=live_pages+63;quota_enforced=true;
    run_fault(&work);
    assert(state->active && state->cr3==fresh.cr3 && state->cr3!=old_cr3 &&
           c.task->waiting_queue && !gate_writes && !oom_calls && !atomic_load(&signals));
    uint64_t flags;kernel_execution_lock_irqsave(&flags);memory_headroom_cancel(c.task);
    c.task->address_space.as=old;address_space_destroy(&fresh);
    kernel_execution_unlock_irqrestore(flags);
    finish_case(&c);puts("PASS changed CR3 discards the old refill identity and cursor");
}

/* A real failed allocation has priority over optional refill; once resolved,
 * a different demand fault can still start headroom under the same task. */
static void fault_priority(void)
{
    struct headroom_case c=make_case(48);
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned before_pages=live_pages;
    run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(probe_allocations==1 && live_pages==before_pages && state->fault_pending &&
           !state->active && c.task->waiting_queue && !gate_writes);
    atomic_store(&fail_alloc,false);run_fault(&work);
    assert(probe_allocations==1 && !state->fault_pending && !c.task->waiting_queue &&
           !atomic_load(&signals) && address_space_user_page_phys(sched_task_as(c.task),c.target));
    work.address+=4096;run_fault(&work);
    assert(probe_allocations==2 && !state->fault_pending && state->active && c.task->waiting_queue &&
           !address_space_user_page_phys(sched_task_as(c.task),work.address));
    uint64_t flags;kernel_execution_lock_irqsave(&flags);memory_headroom_cancel(c.task);
    kernel_execution_unlock_irqrestore(flags);
    finish_case(&c);puts("PASS actual allocation retry has priority and a new fault can refill");
}

/* Registering the first generation for a still-owned root during a pressure
 * wait must not put optional refill ahead of the failed real allocation. */
static void first_generation_priority(void)
{
    struct headroom_case c={.area=setup_area(),.task=new_task(),.target=0x20000000,
                            .victims=0x30000000,.count=4};
    c.task->vmas[0]=(struct task_vma){.used=1,.start=c.target,.end=c.target+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(c.task),c.target,c.target+8192));
    create_victims(c.task,c.victims,c.count);
    assert(!mm_context_generation_existing(sched_task_as(c.task)));
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};
    unsigned before_pages=live_pages;
    struct swap_stats before;swap_get_stats(&before);
    run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->fault_pending && state->wait_owned && !state->generation && !state->active &&
           c.task->waiting_queue && live_pages==before_pages && probe_allocations==1 &&
           !address_space_user_page_phys(sched_task_as(c.task),c.target));

    uint64_t flags,generation=0;kernel_execution_lock_irqsave(&flags);
    assert(!mm_context_ensure_reclaim_generation(sched_task_as(c.task),&generation));
    kernel_execution_unlock_irqrestore(flags);
    assert(generation && !state->generation && state->fault_pending);
    atomic_store(&fail_alloc,false);run_fault(&work);
    uint64_t target=address_space_user_page_phys(sched_task_as(c.task),c.target);
    struct swap_stats after;swap_get_stats(&after);
    fprintf(stderr,"first-generation retry: target=%llu pending=%d active=%d probes=%u queued=%d pages=%u\n",
            (unsigned long long)target,state->fault_pending,state->active,probe_allocations,
            c.task->waiting_queue!=NULL,live_pages-before_pages);
    assert(target && !state->fault_pending && !state->active && !state->wait_owned && !c.task->waiting_queue &&
           probe_allocations==1 && live_pages==before_pages+1 && !gate_writes && !oom_calls &&
           !atomic_load(&signals) && before.total==after.total && before.free==after.free);
    for(unsigned i=0;i<4096;++i)assert(((uint8_t *)mm_phys_to_virt(target))[i]==0);
    finish_case(&c);puts("PASS first generation preserves real fault retry and ownership");
}

/* The priority boundary must not need identity allocation to retry the real
 * fault: no MM context exists at either dispatch in this modeled sequence. */
static void absent_generation_priority(void)
{
    struct headroom_case c={.area=setup_area(),.task=new_task(),.target=0x20000000};
    c.task->vmas[0]=(struct task_vma){.used=1,.start=c.target,.end=c.target+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_prepare_user_range(sched_task_as(c.task),c.target,c.target+8192));
    assert(!mm_context_generation_existing(sched_task_as(c.task)));
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};unsigned pages=live_pages;
    run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->fault_pending && !state->generation && c.task->waiting_queue);
    atomic_store(&fail_alloc,false);run_fault(&work);
    assert(address_space_user_page_phys(sched_task_as(c.task),c.target) &&
           !mm_context_generation_existing(sched_task_as(c.task)) && !state->fault_pending &&
           !state->active && !c.task->waiting_queue && live_pages==pages+1 &&
           probe_allocations==1 && !gate_writes && !oom_calls && !atomic_load(&signals));
    finish_case(&c);puts("PASS retry does not require creating an MM generation");
}

/* A first generation adopted after a wait is no longer a wildcard if the
 * real allocation remains blocked across a later incarnation change. */
static void promoted_generation_change(void)
{
    struct headroom_case c=make_case_no_generation(0);
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->fault_pending && !state->generation && state->wait_owned);
    uint64_t flags,generation=0;kernel_execution_lock_irqsave(&flags);
    assert(!mm_context_ensure_reclaim_generation(sched_task_as(c.task),&generation));
    kernel_execution_unlock_irqrestore(flags);
    run_fault(&work);
    assert(state->fault_pending && state->generation==generation && state->wait_owned &&
           c.task->waiting_queue && probe_allocations==1);
    kernel_execution_lock_irqsave(&flags);
    mm_context_destroy(sched_task_as(c.task));
    uint64_t next=0;assert(!mm_context_ensure_reclaim_generation(sched_task_as(c.task),&next));
    kernel_execution_unlock_irqrestore(flags);
    assert(next && next!=generation);atomic_store(&fail_alloc,false);run_fault(&work);
    assert(!state->fault_pending && state->active && state->generation==next &&
           state->wait_owned && c.task->waiting_queue && probe_allocations==2 &&
           !address_space_user_page_phys(sched_task_as(c.task),c.target));
    finish_case(&c);puts("PASS promoted generation rejects a later changed incarnation");
}

/* Mismatched pending identities must not inherit real-fault priority. */
static void pending_identity_change(const char *change)
{
    bool no_generation=!strcmp(change,"pending-root-zero") || !strcmp(change,"pending-fault-zero");
    struct headroom_case c=no_generation ? make_case_no_generation(0) : make_case(0);
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->fault_pending && (!!state->generation!=no_generation) && c.task->waiting_queue);
    uint64_t old_generation=state->generation,flags;
    struct address_space old=c.task->address_space.as,fresh={0};
    if(!strcmp(change,"pending-generation")) {
        kernel_execution_lock_irqsave(&flags);
        mm_context_destroy(sched_task_as(c.task));
        uint64_t generation=0;assert(!mm_context_ensure_reclaim_generation(sched_task_as(c.task),&generation));
        assert(generation && generation!=old_generation);
        kernel_execution_unlock_irqrestore(flags);
    } else if(!strcmp(change,"pending-root") || !strcmp(change,"pending-root-zero")) {
        atomic_store(&fail_alloc,false);quota_enforced=false;assert(address_space_create(&fresh));
        assert(address_space_prepare_user_range(&fresh,c.target,c.target+8192));
        c.task->address_space.as=fresh;
    } else {
        assert(!strcmp(change,"pending-fault") || !strcmp(change,"pending-fault-zero"));work.address+=4096;
    }
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,false);
    run_fault(&work);
    assert(!state->fault_pending && state->active && c.task->waiting_queue &&
           !address_space_user_page_phys(sched_task_as(c.task),work.address) &&
           probe_allocations==2 && !gate_writes && !oom_calls && !atomic_load(&signals));
    if(!strcmp(change,"pending-generation"))assert(state->generation!=old_generation);
    if(fresh.cr3)assert(state->cr3==fresh.cr3 && fresh.cr3!=old.cr3);
    if(work.address!=c.target)assert(state->page==work.address);
    kernel_execution_lock_irqsave(&flags);memory_headroom_cancel(c.task);
    if(fresh.cr3) { c.task->address_space.as=old;address_space_destroy(&fresh); }
    kernel_execution_unlock_irqrestore(flags);
    finish_case(&c);printf("PASS %s rejects stale real-fault priority\n",change);
}

/* Exercise actual wake and cancellation of the original owned queue link. */
static void owned_wait(bool wake)
{
    struct headroom_case c=make_case(0);
    pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(state->fault_pending && state->wait_owned && c.task->waiting_queue);
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    if(wake)memory_pressure_notify();else memory_headroom_cancel(c.task);
    kernel_execution_unlock_irqrestore(flags);
    assert(!c.task->waiting_queue);
    atomic_store(&fail_alloc,false);
    if(wake) {
        assert(state->fault_pending && c.task->state==TASK_READY);run_fault(&work);
        assert(address_space_user_page_phys(sched_task_as(c.task),c.target));
    }
    assert(!state->fault_pending && !state->wait_owned && !state->active &&
           !c.task->waiting_queue && !gate_writes && !oom_calls && !atomic_load(&signals));
    finish_case(&c);printf("PASS owned pressure wait %s preserves queue and retry state\n",wake ? "wake" : "cancel");
}

/* Successful exec retires the old task/MM token and its stale pressure wait.
 * An unrelated queue must remain owned by its independent operation. */
static void exec_identity(const char *mode)
{
    struct headroom_case c=make_case_no_generation(0);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    cpu=0;current=c.task;current_task[0]=c.task;current_pid[0]=c.task->pid;
    c.task->state=TASK_RUNNING;c.task->running_cpu=0;
    struct kernel_wait_queue other={.lock=KERNEL_SPINLOCK_INIT};
    bool pending=!strcmp(mode,"exec-pending") || !strcmp(mode,"exec-pending-other") ||
                 !strcmp(mode,"exec-pending-pressure");
    bool other_queue=!strcmp(mode,"exec-unrelated") || !strcmp(mode,"exec-pending-other");
    bool other_pressure=!strcmp(mode,"exec-pressure-unrelated") || !strcmp(mode,"exec-pending-pressure");
    if(pending) {
        pressure_baseline_pages=live_pages+10;quota_enforced=true;atomic_store(&fail_alloc,true);
        struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
        assert(state->fault_pending && !state->generation && state->wait_owned && c.task->waiting_queue);
        atomic_store(&fail_alloc,false);quota_enforced=false;
    } else if(!other_pressure) {
        state->cooldown=true;
    }
    if(other_queue) {
        assert(!kernel_wait_queue_add(&other,c.task) && c.task->waiting_queue==&other);
    } else if(other_pressure) {
        c.task->state=TASK_RUNNING;
        uint64_t lock;kernel_execution_lock_irqsave(&lock);
        assert(memory_fault_storage_wait(c.task,c.target)==-EAGAIN);
        kernel_execution_unlock_irqrestore(lock);
        assert(!state->wait_owned);
    }
    struct kernel_wait_queue *original=c.task->waiting_queue;
    struct address_space fresh={0};assert(address_space_create(&fresh));
    uint64_t old_cr3=sched_task_as(c.task)->cr3,flags;
    kernel_execution_lock_irqsave(&flags);
    sched_exec_replace_mm(c.task,&fresh);
    kernel_execution_unlock_irqrestore(flags);
    assert(sched_task_as(c.task)->cr3==fresh.cr3 && fresh.cr3!=old_cr3 &&
           !state->fault_pending && !state->active && !state->cooldown &&
           (other_queue || other_pressure ? c.task->waiting_queue==original : !c.task->waiting_queue));
    if(other_queue)kernel_wait_queue_remove(&other,c.task);
    if(other_pressure)memory_wait_cancel(c.task);
    finish_case(&c);printf("PASS %s clears old identity and preserves unrelated waits\n",mode);
}

static void shared_change(void)
{
    struct headroom_case c=make_case(4);
    pressure_baseline_pages=live_pages+63;quota_enforced=true;
    struct fault_work work={c.task,c.target,6,0x23,0,NULL,0};run_fault(&work);
    assert(c.task->waiting_queue && sched_pressure_headroom_state(c.task)->active);
    c.task->shared_mm=&c.task->address_space;
    run_fault(&work);
    struct pressure_headroom_state *state=sched_pressure_headroom_state(c.task);
    assert(!state->active && !state->cooldown && !c.task->waiting_queue &&
           !atomic_load(&signals) && !gate_writes && !oom_calls &&
           address_space_user_page_phys(sched_task_as(c.task),c.target));
    c.task->shared_mm=NULL;finish_case(&c);
    puts("PASS shared-MM transition cancels refill and resolves the real fault");
}

int main(int argc,char **argv)
{
    assert(argc==2);nx_enabled=true;page_cache_init();
    if(!strcmp(argv[1],"name-churn")) {
        churn_names=true;early_refill();assert(name_evictions);
        puts("PASS continuous name-cache churn cannot starve page/swap reclaim");
    }
    else if(!strcmp(argv[1],"early"))early_refill();
    else if(!strcmp(argv[1],"above-low"))above_low();
    else if(!strcmp(argv[1],"partial"))partial_fallback();
    else if(!strcmp(argv[1],"swapon"))swap_activation();
    else if(!strcmp(argv[1],"high-recovery"))high_recovery();
    else if(!strcmp(argv[1],"protection"))protection(false);
    else if(!strcmp(argv[1],"cancel"))protection(true);
    else if(!strcmp(argv[1],"shared-change"))shared_change();
    else if(!strcmp(argv[1],"fault-priority"))fault_priority();
    else if(!strcmp(argv[1],"first-generation-priority"))first_generation_priority();
    else if(!strcmp(argv[1],"absent-generation-priority"))absent_generation_priority();
    else if(!strcmp(argv[1],"promoted-generation-change"))promoted_generation_change();
    else if(!strcmp(argv[1],"pending-generation") || !strcmp(argv[1],"pending-root") ||
            !strcmp(argv[1],"pending-fault") || !strcmp(argv[1],"pending-root-zero") ||
            !strcmp(argv[1],"pending-fault-zero"))pending_identity_change(argv[1]);
    else if(!strcmp(argv[1],"owned-wake"))owned_wait(true);
    else if(!strcmp(argv[1],"owned-cancel"))owned_wait(false);
    else if(!strcmp(argv[1],"exec-pending") || !strcmp(argv[1],"exec-unrelated") ||
            !strcmp(argv[1],"exec-pending-other") || !strcmp(argv[1],"exec-pressure-unrelated") ||
            !strcmp(argv[1],"exec-pending-pressure"))
        exec_identity(argv[1]);
    else if(!strcmp(argv[1],"root-change"))root_change();
    else if(!strcmp(argv[1],"write-eio"))storage_fallback(false);
    else if(!strcmp(argv[1],"write-contended"))storage_fallback(true);
    else abort();
    return 0;
}

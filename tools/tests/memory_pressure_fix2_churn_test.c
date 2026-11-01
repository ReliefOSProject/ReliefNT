/* Actual PTE mapping, DEVICE revision and OOM selector. RAM frames, task slots,
 * the scheduler/CPU and signal delivery are finite deterministic host models. */
#include "../../kernel/sched/internal.h"
#define PRESSURE_CUSTOM_TASK_TABLE 1
#define SWAP_PAGING_FIXTURE_ENTRY inherited_swap_paging_main
#define current_task fixture_current_task
#include "swap_paging_test.c"
#undef current_task
#include "../../mm/reclaim/internal.h"
#include <reliefnt/memory_pressure.h>

static struct sched_task_object candidates[3];
static unsigned charged_work,killed[2];

/** @brief Charge a scheduler-slot inspection to the selector's budget.
 * @param index Modeled slot. @return Borrowed task or NULL at table end.
 */
struct task *sched_task_at(uint32_t index)
{ ++charged_work;return index<3?&candidates[index].task:NULL; }
/** @brief Give the selector a fixed model capacity. @return Capacity in KiB. */
uint64_t mm_total_memory_kib(void) { return 100*4; }
/** @brief Enter the real bounded PTE iterator through the linker wrapper.
 * @param as Borrowed root. @param cursor In/out address. @param page Output leaf.
 * @return The iterator's unmodified result.
 */
int __real_paging_user_pressure_step(struct address_space *as,uint64_t *cursor,uint64_t *page);
/** @brief Charge a real PTE iterator step without changing its result.
 * @param as Borrowed root. @param cursor In/out address. @param page Output leaf.
 * @return The real iterator result.
 */
int __wrap_paging_user_pressure_step(struct address_space *as,uint64_t *cursor,uint64_t *page)
{ ++charged_work;return __real_paging_user_pressure_step(as,cursor,page); }
/** @brief Model process-directed SIGKILL publication for the two candidates.
 * @param task Borrowed model target. @param process Must be process-directed.
 * @param signal Must be SIGKILL. @param info Must be NULL. @return Zero.
 */
int kernel_signal_enqueue(struct task *task,bool process,int signal,const struct linux_siginfo *info)
{
    assert(process && signal==9 && !info && task->pid>=3 && task->pid<=4);
    ++killed[task->pid-3];*sched_task_process_pending(task)|=1ULL<<8;return 0;
}
/** @brief Model a wake for already runnable fixture tasks.
 * @param task Borrowed task; this host model does not execute a guest CPU.
 */
void sched_wake_fatal(struct task *task) { assert(task); }

/** @brief Perform one real selector call and enforce its production budget.
 * @param budget Allowed inspected work. @return Actual selector status.
 */
static int step(uint32_t budget)
{ charged_work=0;int rc=memory_oom_advance(budget);assert(charged_work<=budget);return rc; }

/** @brief Test repeated real DEVICE or eligibility changes between 32-unit batches.
 * @param argc Argument count. @param argv Names the churn/static scenario.
 * @return Zero after bounded outcome, correct static score and lifetime checks.
 */
int main(int argc,char **argv)
{
    assert(argc==2); bool device=!strcmp(argv[1],"device");
    bool none=!strcmp(argv[1],"device-no-victim");
    bool membership=!strcmp(argv[1],"membership");
    bool stable=!strcmp(argv[1],"stable");
    assert(device||none||membership||stable);
    nx_enabled=true;
    struct task *a=&candidates[0].task,*b=&candidates[1].task,*guard=&candidates[2].task;
    a->pid=3;b->pid=4;guard->pid=5;a->kind=b->kind=TASK_KIND_USER;
    a->state=b->state=TASK_READY;
    assert(address_space_create(sched_task_as(a)) && address_space_create(sched_task_as(b)));
    uint64_t generation;
    assert(!mm_context_ensure_generation(sched_task_as(a),&generation));
    assert(!mm_context_ensure_generation(sched_task_as(b),&generation));
    for (unsigned i=0;i<2;++i) {
        struct task_identity *id=sched_task_identity_prepare();assert(id);
        sched_task_identity_bind(&candidates[i].task,id);
    }
    if (none) {
        guard->kind=TASK_KIND_USER;guard->state=TASK_READY;guard->flags=TASK_FLAG_SERVICE;
        guard->shared_mm=&a->address_space;b->flags|=TASK_FLAG_SERVICE;
    }
    const uint64_t ram=RELIEFNT_USER_BASE+0x10000,dev=ram+0x200000;
    uint64_t page=mm_alloc_page();assert(page);
    assert(address_space_map_user_page(sched_task_as(a),ram,page,RELIEFNT_PAGE_WRITABLE));
    unsigned b_pages=stable?300:4;
    for (unsigned i=0;i<b_pages;++i) {
        if (stable && i) mm_retain_page(page);
        else { page=mm_alloc_page();assert(page); }
        assert(address_space_map_user_page(sched_task_as(b),ram+i*4096,page,RELIEFNT_PAGE_WRITABLE));
    }
    /* Prepare the page tables once; subsequent DEVICE churn is allocation-free. */
    assert(address_space_map_user_page(sched_task_as(a),dev,0x10000000ULL,RELIEFNT_PAGE_DEVICE));
    assert(address_space_unmap_user_page(sched_task_as(a),dev)==0x10000000ULL);
    assert(oom_score_for_task(a)==10 && (none || oom_score_for_task(b)==(stable?1000:40)));
    if (stable) assert(address_space_user_resident_kib(sched_task_as(b))==300*4);
    assert(step(1)==-EAGAIN);
    bool mapped=false,terminated=false;unsigned calls=1,mutations=0,ordinary_progress=0;
    if (!stable) {
        for (unsigned i=0;i<40;++i) {
            if (device||none) {
                if (mapped) { assert(address_space_unmap_user_page(sched_task_as(a),dev)==0x10000000ULL);mapped=false; }
                else { assert(address_space_map_user_page(sched_task_as(a),dev,0x10000000ULL,RELIEFNT_PAGE_DEVICE));mapped=true; }
            } else sched_oom_policy_set(b,(i&1)?0:1,0);
            ++mutations;
            int rc=step(32);++calls;
            assert(!killed[0] && !killed[1]);
            if (rc==-ENOMEM) { terminated=true;break; }
            assert(rc==-EAGAIN);
        }
        if (!terminated) fprintf(stderr,"unbounded churn mode=%s mutations=%u work<=32\n",argv[1],mutations);
        assert(terminated);
    }
    /* With changes stopped, a correct RAM score picks the four-page candidate;
     * if its only peer root has a protected holder, no victim can be selected. */
    int rc=-EAGAIN;
    do {
        rc=step(32);++calls;
        if (rc==-EAGAIN) ++ordinary_progress;
        assert(calls<3000 && !killed[0]);
    } while (rc==-EAGAIN && !killed[1]);
    if (none) assert(rc==-ENOMEM && !killed[1]);
    else assert((rc==-EAGAIN || rc==-EINPROGRESS) && killed[1]==1 && ordinary_progress>8);
    assert(oom_score_for_task(a)==10 && (none || oom_score_for_task(b)==(stable?1000:40)));
    a->flags|=TASK_FLAG_SERVICE;b->flags|=TASK_FLAG_RESOURCES_RELEASED;
    if (!none) {
        do {rc=step(32);assert(++calls<3100);} while (rc==-EAGAIN || rc==-EINPROGRESS);
        assert(rc==-ENOMEM);
    }
    sched_task_identity_detach(a);sched_task_identity_detach(b);
    address_space_destroy(sched_task_as(a));address_space_destroy(sched_task_as(b));
    assert(!allocated);
    printf("PASS mode=%s churn=%u terminal=%u static_progress=%u ram_leaves=%u budget=32 batches=%u\n",
           argv[1],mutations,(unsigned)terminated,ordinary_progress,b_pages,calls);
    return 0;
}

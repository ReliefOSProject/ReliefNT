/* Real PTE map/clone/protect/unmap, swap drain and OOM selector. Physical RAM,
 * device PFNs, task table, signal delivery and disk bytes are host models. */
#include "../../kernel/sched/internal.h"
#define PRESSURE_CUSTOM_TASK_TABLE 1
#define SWAP_PAGING_FIXTURE_ENTRY inherited_swap_paging_main
#define current_task fixture_current_task
#include "swap_paging_test.c"
#undef current_task
#include "../../mm/reclaim/internal.h"
#include <reliefnt/memory_pressure.h>

static struct sched_task_object candidates[2];
static unsigned charged_work, killed[2];
static uint64_t first_root_cursor;
static uint64_t reported_total_kib = 100 * 4;
extern int oom_proc_score_for_task(struct task *) __attribute__((weak));
/** @brief Charge a task-slot inspection to the tested selection budget.
 * @param index Scheduler slot. @return Borrowed modeled task or NULL at end.
 */
struct task *sched_task_at(uint32_t index)
{ ++charged_work; return index < 2 ? &candidates[index].task : NULL; }
/** @brief Model capacity for score normalization. @return Fixed KiB capacity. */
uint64_t mm_total_memory_kib(void) { return reported_total_kib; }
/** @brief Call the real bounded PTE iterator through the linker wrapper.
 * @param as Borrowed MM. @param cursor In/out address. @param page Output leaf.
 * @return Unmodified iterator state; no ownership changes.
 */
int __real_paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page);
/** @brief Charge one actual PTE walk step and record the first root cursor.
 * @param as Borrowed MM. @param cursor In/out address. @param page Output leaf.
 * @return Actual iterator state with no altered PTE behavior.
 */
int __wrap_paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page)
{
    ++charged_work;
    int state=__real_paging_user_pressure_step(as,cursor,page);
    if (as==sched_task_as(&candidates[0].task)) first_root_cursor=*cursor;
    return state;
}
/** @brief Model successful process SIGKILL publication and count its target.
 * @param task Borrowed modeled task. @param process Must be process-directed.
 * @param signal Must be SIGKILL. @param info Must be NULL. @return Zero.
 */
int kernel_signal_enqueue(struct task *task, bool process, int signal,
                          const struct linux_siginfo *info)
{
    assert(process && signal == 9 && !info && task->pid >= 3 && task->pid <= 4);
    ++killed[task->pid - 3];
    *sched_task_process_pending(task) |= 1ULL << 8;
    return 0;
}
/** @brief Model wakeup of an already runnable fixture task.
 * @param task Borrowed modeled task; this fixture does not simulate CPU execution.
 */
void sched_wake_fatal(struct task *task) { (void)task; }

/** @brief Test DEVICE exclusion through actual PTE and slot transitions.
 * @return Zero after score, selector, bytes, budget and ownership checks.
 */
int main(void)
{
    nx_enabled = true;
    struct task *a = &candidates[0].task, *b = &candidates[1].task;
    a->pid=3; b->pid=4; a->kind=b->kind=TASK_KIND_USER;
    a->state=b->state=TASK_READY;
    assert(address_space_create(sched_task_as(a)) && address_space_create(sched_task_as(b)));
    uint64_t generation;
    assert(!mm_context_ensure_generation(sched_task_as(a), &generation));
    assert(!mm_context_ensure_generation(sched_task_as(b), &generation));
    for (unsigned i=0;i<2;++i) {
        struct task_identity *id=sched_task_identity_prepare(); assert(id);
        sched_task_identity_bind(&candidates[i].task,id);
    }
    const uint64_t ram=RELIEFNT_USER_BASE+0x10000, device=ram+0x200000;
    a->vmas[0]=(struct task_vma){.used=1,.start=ram,.end=ram+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t frame=mm_alloc_page(); assert(frame); memset(mm_phys_to_virt(frame),0x5a,4096);
    assert(address_space_map_user_page(sched_task_as(a),ram,frame,RELIEFNT_PAGE_WRITABLE));
    for (unsigned i=0;i<10;++i)
        assert(address_space_map_user_page(sched_task_as(a),device+i*4096,
            0x10000000ULL+i*4096,RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_NOEXEC));
    for (unsigned i=0;i<4;++i) {
        uint64_t page=mm_alloc_page(); assert(page);
        assert(address_space_map_user_page(sched_task_as(b),ram+i*4096,page,RELIEFNT_PAGE_WRITABLE));
    }
    printf("initial DEVICE-holder score=%d RAM-holder score=%d mapped=%llu resident_kib=%llu\n",
        oom_score_for_task(a),oom_score_for_task(b),
        (unsigned long long)sched_task_as(a)->user_page_count,
        (unsigned long long)address_space_user_resident_kib(sched_task_as(a))); fflush(stdout);
    assert(sched_task_as(a)->user_page_count==11);
    assert(address_space_user_resident_kib(sched_task_as(a))==4);
    assert(oom_score_for_task(a)==10 && oom_score_for_task(b)==40);
    if (!oom_proc_score_for_task) fprintf(stderr,"standard proc OOM score producer is absent\n");
    assert(oom_proc_score_for_task);
    assert(oom_proc_score_for_task(a)==673 && oom_proc_score_for_task(b)==693);
    reported_total_kib=1000*4;
    sched_oom_policy_set(a,-500,0);
    assert(oom_proc_score_for_task(a)==334);
    sched_oom_policy_set(a,-1000,-1000);
    assert(oom_proc_score_for_task(a)==0);
    sched_oom_policy_set(a,0,0); reported_total_kib=0;
    assert(oom_proc_score_for_task(a)==0);
    reported_total_kib=UINT64_MAX;
    assert(oom_proc_score_for_task(a)==666);
    reported_total_kib=100*4;
    assert(address_space_protect_user_page(sched_task_as(a),ram,RELIEFNT_PAGE_PROTNONE));
    assert(address_space_protect_user_page(sched_task_as(a),device,RELIEFNT_PAGE_PROTNONE));
    assert(oom_score_for_task(a)==10);

    struct sched_task_object child={0}; child.task.kind=TASK_KIND_USER; child.task.pid=8;
    assert(address_space_clone_cow(sched_task_as(a),sched_task_as(&child.task)));
    assert(sched_task_as(&child.task)->user_page_count==11 && oom_score_for_task(&child.task)==10);
    assert(address_space_unmap_user_page(sched_task_as(&child.task),device)==0x10000000ULL);
    assert(oom_score_for_task(&child.task)==10);
    address_space_destroy(sched_task_as(&child.task));
    assert(address_space_unmap_user_page(sched_task_as(a),device)==0x10000000ULL);
    assert(oom_score_for_task(a)==10);
    assert(address_space_protect_user_page(sched_task_as(a),ram,RELIEFNT_PAGE_WRITABLE));

    uint8_t header[4096]={0}; header[1024]=1; header[1028]=3;
    memcpy(header+4086,"SWAPSPACE2",10);
    struct swap_area *area=NULL; assert(!swap_area_create(header,sizeof(header),4,1,&area));
    assert(!swap_area_publish(area)); fixture_current_task=a;
    assert(swap_try_evict(sched_task_as(a),ram)==1);
    assert(address_space_user_resident_kib(sched_task_as(a))==0);
    int swapped_score=oom_score_for_task(a); assert(swapped_score>0);
    assert(!swap_area_begin_drain(area));
    struct swap_drain_cursor cursor={0}; unsigned batches=0; int rc;
    do { uint32_t work=0; rc=swap_drain_step(area,&cursor,32,&work);
         assert(work<=32 && rc>=0 && ++batches<100); } while (!rc);
    assert(address_space_user_resident_kib(sched_task_as(a))==4);
    frame=address_space_user_page_phys(sched_task_as(a),ram); assert(frame);
    for (unsigned i=0;i<4096;++i) assert(((uint8_t *)mm_phys_to_virt(frame))[i]==0x5a);
    swap_area_destroy(area);
    assert(oom_score_for_task(a)==10 && oom_score_for_task(b)==40);

    unsigned calls=0; bool inserted_behind_cursor=false;
    while (!killed[1]) {
        charged_work=0; rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && charged_work<=2 && ++calls<10000);
        assert(!killed[0]);
        if (!inserted_behind_cursor && first_root_cursor > device+20*4096 &&
            first_root_cursor < RELIEFNT_USER_TOP) {
            uint64_t revision=mm_context_device_revision(sched_task_as(a));
            for (unsigned i=10;i<14;++i)
                assert(address_space_map_user_page(sched_task_as(a),device+i*4096,
                    0x10000000ULL+i*4096,RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_NOEXEC));
            assert(mm_context_device_revision(sched_task_as(a))==revision+4);
            assert(oom_score_for_task(a)==10);
            inserted_behind_cursor=true;
        }
    }
    assert(inserted_behind_cursor && killed[1]==1 && !killed[0]);
    a->flags|=TASK_FLAG_SERVICE; b->flags|=TASK_FLAG_RESOURCES_RELEASED;
    do { charged_work=0; rc=memory_oom_advance(2); assert(charged_work<=2 && ++calls<11000); }
    while (rc==-EAGAIN || rc==-EINPROGRESS);
    assert(rc==-ENOMEM);
    sched_task_identity_detach(a); sched_task_identity_detach(b);
    address_space_destroy(sched_task_as(a)); address_space_destroy(sched_task_as(b));
    assert(!allocated);
    printf("PASS DEVICE excluded from real PTE score/selection and clone/protect/swapoff; mutation restarted walk (%u batches)\n",calls);
    return 0;
}

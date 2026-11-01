/* Actual user trap, PTEs, pressure engine, wait queue, selector and PID objects.
 * Task table, CPU handoff, physical quota and victim retirement are host models. */
#define _POSIX_C_SOURCE 200809L
#include "../../kernel/sched/internal.h"
#define PRESSURE_REAL_FIXTURE 1
#define PRESSURE_FAULT_TEST 1
#define PRESSURE_CUSTOM_TASK_TABLE 1
#define SWAP_FAULT_GATE_FIXTURE_ENTRY inherited_vfork_gate_main
#include "swap_fault_gate_test.c"
#include "../../mm/reclaim/internal.h"

static struct task *members[4], *alternative;
static unsigned victim_signals;
static bool quota_released;
static void (*retirement_notify)(void);

/** @brief Expose the vfork family, independent same-MM peer and other MM.
 * @param index Slot index. @return Borrowed task or NULL at the table end.
 */
struct task *sched_task_at(uint32_t index)
{ return index < 4 ? members[index] : NULL; }
/** @brief Model installed physical memory. @return Fixed capacity in KiB. */
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
/** @brief Model the emergency reserve. @return Fixed reserve in pages. */
uint64_t mm_emergency_reserve_pages(void) { return 64; }
/** @brief Raise free space only after the modeled alternative victim retires.
 * @return Free KiB at the reserve before, one ordinary page above after.
 */
uint64_t mm_free_memory_kib(void) { return 4 * (64 + (unsigned)quota_released); }
/** @brief Model an idle storage backend. @return False. */
bool swap_storage_busy(void) { return false; }
/** @brief Capture the real pressure waiter's resource-retirement notification.
 * @param notify Borrowed callback retained until the modeled retirement.
 */
void sched_set_memory_release_notify(void (*notify)(void)) { retirement_notify=notify; }
/** @brief Accept a real selector SIGKILL only for the independent candidate.
 * @param task Borrowed target. @param process Must be process-directed.
 * @param signal Must be SIGKILL. @param info Must be NULL. @return Zero.
 * The signal queue and CPU execution are modeled; selection itself is real.
 */
int kernel_signal_enqueue(struct task *task, bool process, int signal,
                          const struct linux_siginfo *info)
{
    assert(task==alternative && process && signal==9 && !info);
    ++victim_signals; *sched_task_process_pending(task) |= 1ULL << 8;
    return 0;
}

/** @brief Drive a vfork child's real fault until another MM is selected.
 * @return Zero after the selected model victim retires and the fault resolves.
 */
int main(void)
{
    nx_enabled=true; page_cache_init();
    struct task *child=new_task();
    struct task *parent=calloc(1,sizeof(struct sched_task_object));
    struct task *peer=calloc(1,sizeof(struct sched_task_object));
    alternative=new_task(); assert(parent && peer);
    child->pid=3; parent->pid=4; peer->pid=5; alternative->pid=6;
    parent->kind=peer->kind=TASK_KIND_USER;
    child->shared_mm=parent->shared_mm=peer->shared_mm=&child->address_space;
    child->vfork_parent=parent; parent->vfork_child=child;
    child->state=TASK_RUNNING; parent->state=TASK_BLOCKED;
    peer->state=alternative->state=TASK_READY;
    members[0]=peer; members[1]=parent; members[2]=child; members[3]=alternative;
    const uint64_t va=RELIEFNT_USER_BASE+0x50000;
    child->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t frame=mm_alloc_page(); assert(frame); memset(mm_phys_to_virt(frame),0x7a,4096);
    assert(address_space_map_user_page(sched_task_as(alternative),va,frame,RELIEFNT_PAGE_WRITABLE));
    uint64_t generation;
    assert(!mm_context_ensure_generation(sched_task_as(child),&generation));
    assert(!mm_context_ensure_generation(sched_task_as(alternative),&generation));
    for (unsigned i=0;i<4;++i) {
        struct task_identity *id=sched_task_identity_prepare(); assert(id);
        sched_task_identity_bind(members[i],id);
    }
    atomic_store(&fail_alloc,true);
    struct fault_work work={child,va,6,0x23,0,NULL,0};
    unsigned attempts=0;
    while (!victim_signals) {
        run_fault(&work);
        assert(++attempts<3000 && !atomic_load(&signals));
        if (!victim_signals) assert(memory_reclaim_stall_cause()==PRESSURE_STALL_NONE);
    }
    assert(attempts>8);
    assert(victim_signals==1 && parent->state==TASK_BLOCKED && !child->pending_signals &&
           !peer->pending_signals && retirement_notify);
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    atomic_store(&fail_alloc,false); quota_released=true;
    retirement_notify();
    run_fault(&work);
    frame=address_space_user_page_phys(sched_task_as(child),va);
    assert(frame && address_space_user_page_writable(sched_task_as(child),va));
    assert(!atomic_load(&signals) && !child->waiting_queue);
    for (unsigned i=0;i<4096;++i) assert(!((unsigned char *)mm_phys_to_virt(frame))[i]);
    for (unsigned i=0;i<4;++i) sched_task_identity_detach(members[i]);
    child->shared_mm=NULL;
    destroy_task(alternative); destroy_task(child); free(parent); free(peer);
    assert(!live_pages);
    printf("PASS real vfork-child fault selects independent MM and resolves after modeled retirement (%u attempts)\n",attempts);
    return 0;
}

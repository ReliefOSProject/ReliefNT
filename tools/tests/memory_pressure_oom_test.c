/* Production OOM selection, scheduler-private policy and PID objects. The task
 * table, MM counts and signal/wake endpoints are a finite deterministic model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/memory_stats.h>
#include <reliefnt/swap.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include "../../kernel/sched/internal.h"
#include "../../mm/reclaim/internal.h"

static struct sched_task_object objects[6];
static struct task_address_space_state spaces[6];
static bool allocation_allowed = true;
static unsigned inspections, sent[10], wakes, freed_identities;
static unsigned oom_logs, oom_log_pid;

/** @brief Capture the one committed OOM publication diagnostic and its PID. */
void console_log(enum console_log_level level,const char *format,...)
{
    assert(level==CONSOLE_LOG_WARN && strstr(format,"oom kill signal published pid="));
    va_list args; va_start(args,format); oom_log_pid=va_arg(args,unsigned); va_end(args); ++oom_logs;
}

void *kernel_malloc(size_t n) { assert(allocation_allowed); return malloc(n); }
void kernel_free(void *p) { if (p) ++freed_identities; free(p); }
bool kernel_execution_is_outermost(void) { return true; }
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
/** @brief Model a fixture with only owned RAM leaves.
 * @param as Borrowed model MM or NULL. @return Modeled resident size in KiB.
 */
uint64_t address_space_user_resident_kib(const struct address_space *as)
{ return as ? as->user_page_count * 4 : 0; }
/** @brief Model a fixture with no mutable DEVICE mappings.
 * @param as Borrowed model MM. @return Constant revision zero.
 */
uint64_t mm_context_device_revision(const struct address_space *as) { (void)as; return 0; }
/** @brief Terminate a DEVICE-only scan because this model has no DEVICE leaves.
 * @param as Borrowed model MM. @param cursor Unchanged in/out address.
 * @param page Unchanged output. @return Zero, meaning end of scan.
 */
int paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page)
{ (void)as; (void)cursor; (void)page; return 0; }
/** @brief Fail closed if the empty DEVICE scan unexpectedly returns a leaf.
 * @param as Borrowed model MM. @param page Unexpected leaf address.
 * @return Never; the fixture aborts instead of fabricating flags.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
uint64_t mm_swap_count_existing(const struct address_space *as) { (void)as; return 0; }
uint64_t mm_context_generation_existing(const struct address_space *as)
{ return as && as->cr3 ? as->cr3 ^ 0x55 : 0; }
void swap_get_stats(struct swap_stats *out) { *out=(struct swap_stats){.total=8192}; }
struct task *sched_task_at(uint32_t i)
{ ++inspections; return i < 6 ? &objects[i].task : NULL; }
int kernel_signal_enqueue(struct task *task, bool process, int sig, const struct linux_siginfo *info)
{
    assert(process && sig == 9 && !info && task->pid < 10);
    assert(!(*sched_task_process_pending(task) & (1ULL << 8)));
    *sched_task_process_pending(task) |= 1ULL << 8; ++sent[task->pid]; return 0;
}
void sched_wake_fatal(struct task *task)
{ ++wakes; if (task->state == TASK_STOPPED || task->state == TASK_BLOCKED) task->state = TASK_READY; }

static struct task *item(unsigned i, uint32_t pid, unsigned space, uint64_t pages)
{
    struct task *task = &objects[i].task;
    task->pid=pid; task->kind=TASK_KIND_USER; task->state=TASK_READY;
    task->shared_mm=&spaces[space]; spaces[space].as.cr3=(space+1)*4096;
    spaces[space].as.user_page_count=pages;
    struct task_identity *identity=sched_task_identity_prepare(); assert(identity);
    sched_task_identity_bind(task, identity);
    return task;
}

static void release_identities(void)
{ for (unsigned i=0;i<6;++i) sched_task_identity_detach(&objects[i].task); }

static void policy(void)
{
    struct task *a=item(0,3,0,300), *b=item(1,4,0,300), *init=item(2,1,0,300);
    struct task *vf=item(3,5,0,300); vf->vfork_parent=a;
    struct task *other=item(4,6,1,10), *priv=item(5,7,2,10);
    priv->cap_effective=1ULL<<CAP_SYS_RESOURCE;
    assert(!oom_adj_set(a,500,a));
    assert(oom_adj_get(a)==500 && oom_adj_get(b)==500);
    assert(oom_adj_get(init)==0 && oom_adj_get(vf)==0 && oom_adj_get(other)==0);
    assert(oom_adj_set(a,-1,a)==-EACCES);
    assert(!oom_adj_set(a,-500,priv));
    int adjustment, minimum; sched_oom_policy_get(b,&adjustment,&minimum);
    assert(adjustment==-500 && minimum==-500);
    assert(!oom_adj_set(a,0,a) && oom_adj_set(a,-501,a)==-EACCES);
    assert(oom_adj_set(a,1001,priv)==-EINVAL);
    assert(!oom_adj_set(vf,700,vf));
    assert(oom_adj_get(vf)==700 && oom_adj_get(a)==0);
    sched_oom_policy_inherit(other,a);
    sched_oom_policy_get(other,&adjustment,&minimum);
    assert(adjustment==0 && minimum==-500);
    assert(oom_score_for_task(a)==oom_score_for_task(b));
    assert(oom_score_for_task(init)==0 && oom_score_for_task(vf)==0);
    release_identities(); puts("PASS capability minimum, inheritance and shared-MM/vfork adjustment");
}

static void selection(void)
{
    struct task *init=item(0,1,0,8000), *service=item(1,2,1,7000);
    struct task *a=item(2,3,2,300), *b=item(3,4,2,300);
    struct task *small=item(4,5,3,100), *vf=item(5,6,2,300);
    /* The third holder has exited but has not released its MM yet. A vfork
     * holder is separately required to veto this MM in the regression test. */
    service->flags |= TASK_FLAG_SERVICE; b->state=TASK_STOPPED; vf->state=TASK_EXITED;
    (void)init; (void)small;
    allocation_allowed=false;
    unsigned calls=0;
    while (!(sent[3] && sent[4])) {
        inspections=0; int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && inspections<=2 && ++calls<200);
    }
    assert(!sent[1] && !sent[2] && !sent[5] && !sent[6]);
    assert(sent[3]==1 && sent[4]==1 && b->state==TASK_READY);
    struct memory_events events; memory_events_get(&events);
    assert(events.value[MEMORY_EVENT_OOM_KILL]==1 && oom_logs==1 && oom_log_pid==3);
    for (unsigned i=0;i<50;++i) {
        inspections=0; int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && inspections<=2);
    }
    assert(sent[3]==1 && sent[4]==1 && !sent[5] && wakes>=2);
    memory_events_get(&events);
    assert(events.value[MEMORY_EVENT_OOM_KILL]==1 && oom_logs==1);
    a->state=TASK_EXITED; a->flags|=TASK_FLAG_RESOURCES_RELEASED;
    sched_task_identity_detach(a);
    unsigned freed_before=freed_identities;
    for (unsigned i=0;i<12;++i) { int rc=memory_oom_advance(2); assert(rc==-EAGAIN || rc==-EINPROGRESS); }
    assert(freed_identities==freed_before && !sent[5]);
    b->state=TASK_EXITED; b->flags|=TASK_FLAG_RESOURCES_RELEASED;
    for (unsigned i=0;i<12;++i) { int rc=memory_oom_advance(2); assert(rc==-EAGAIN || rc==-EINPROGRESS); }
    assert(freed_identities==freed_before && !sent[5]);
    vf->state=TASK_EXITED; vf->flags|=TASK_FLAG_RESOURCES_RELEASED;
    unsigned retirement_calls=0;
    while (freed_identities==freed_before) {
        assert(memory_oom_advance(2)==-EAGAIN && ++retirement_calls<10);
    }
    assert(!sent[5]);
    allocation_allowed=true;
    release_identities();
    printf("PASS bounded victim selection and in-flight guard after %u calls\n", calls);
}

static void protected_only(void)
{
    struct task *a=item(0,1,0,1000), *b=item(1,2,1,1000), *c=item(2,3,1,1000);
    struct task *d=item(3,4,3,1000), *e=item(4,5,4,1000), *f=item(5,6,5,1000);
    (void)a; (void)c; b->flags|=TASK_FLAG_SERVICE; f->vfork_parent=d;
    sched_oom_policy_set(d,-1000,-1000); sched_oom_policy_set(e,-1000,-1000);
    sched_oom_policy_set(f,-1000,-1000);
    allocation_allowed=false;
    int rc; unsigned calls=0;
    do { inspections=0; rc=memory_oom_advance(2); assert(inspections<=2 && ++calls<100); }
    while (rc==-EAGAIN);
    assert(rc==-ENOMEM);
    for (unsigned i=0;i<10;++i) assert(!sent[i]);
    struct memory_events events; memory_events_get(&events);
    assert(!events.value[MEMORY_EVENT_OOM_KILL] && !oom_logs);
    allocation_allowed=true; release_identities();
    puts("PASS protected-only OOM terminates without a victim");
}

int main(int argc, char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"policy")) policy();
    else if (!strcmp(argv[1],"selection")) selection();
    else if (!strcmp(argv[1],"protected")) protected_only();
    else assert(!"unknown OOM case");
    return 0;
}

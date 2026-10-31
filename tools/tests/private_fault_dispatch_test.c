/* Real dispatcher, scheduler return predicate, execution gate, paging and
 * page cache with host page/CPU/provider and scheduler-handoff boundaries. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <reliefnt/arch.h>
#include <reliefnt/lock.h>

#define RELIEFNT_TEST_REAL_EXEC_SOURCE
#include "../../arch/x86_64/mm/paging.c"
#include "../../kernel/sync/lock.c"
#include "modules/user_memory.h"
#include "sysv_shm_fixture_stubs.h"
#include "exec_file_fixture.h"

struct task *page_fault_dispatch(struct trap_frame *frame);
static _Thread_local uint32_t cpu;
/* An x86 interrupt gate masks IF before page_fault_dispatch is called. */
static _Thread_local uint64_t irq_flags;
static _Thread_local uint64_t fault_address;
static _Thread_local struct task *current;
static _Thread_local int mapping_owner;
static _Thread_local struct trap_frame *live_frame;
static atomic_uint schedules, signals, polls, provider_reads;
static atomic_bool fail_alloc;
static pthread_mutex_t page_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t phys; unsigned refs; } pages[512];
static unsigned live_pages;
static unsigned char file_bytes[3 * 4096];
static struct task next_task;
extern struct task *current_task[SMP_MAX_CPUS];
extern uint32_t current_pid[SMP_MAX_CPUS];
static bool preserve_sched_state, bad_frame, bad_reservation, require_no_resched;
static void wait_until(atomic_uint *value, unsigned target);
static atomic_uint notify_in_mapping, notify_mapping_entered, notify_mapping_done;

static bool reader_owned(void)
{
    return __atomic_load_n(&execution_readers, __ATOMIC_SEQ_CST) != 0 &&
           __atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == UINT32_MAX;
}
static bool writer_owned(void)
{
    return __atomic_load_n(&execution_owner, __ATOMIC_RELAXED) == cpu &&
           execution_depth[cpu] != 0;
}
struct vfs_exec_source *__real_vfs_exec_source_get(struct vfs_exec_source *source);
void __real_vfs_exec_source_put(struct vfs_exec_source *source);
struct vfs_exec_source *__wrap_vfs_exec_source_get(struct vfs_exec_source *source)
{ assert(!reader_owned()); return __real_vfs_exec_source_get(source); }
void __wrap_vfs_exec_source_put(struct vfs_exec_source *source)
{ assert(!reader_owned()); __real_vfs_exec_source_put(source); }
uint32_t smp_current_cpu(void) { return cpu; }
uint32_t smp_cpu_count(void) { return 4; }
bool smp_cpu_online(uint32_t index) { return index < 4; }
void smp_membarrier_poll(void) { atomic_fetch_add(&polls, 1); }
uint64_t kernel_irq_save(void) { uint64_t old = irq_flags; irq_flags = 0; return old; }
void kernel_irq_restore(uint64_t flags) { irq_flags = flags; }
struct task *sched_find(uint32_t pid) { return current && current->pid == pid ? current : NULL; }
uint64_t x86_64_read_cr2(void) { return fault_address; }
uint64_t time_ticks(void) { return 0; }
void smp_flush_user_tlb(void) { }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
void *kernel_malloc(size_t size) { return calloc(1, size); }
void kernel_free(void *p) { free(p); }
uint32_t sched_task_vma_capacity(const struct task *task)
{ (void)task; return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{ assert(index < SCHED_TASK_VMA_MAX); return &sched_task_mm(task)->vmas[index]; }
struct task *userland_schedule_from_frame(struct trap_frame *frame)
{
    assert(frame && frame == live_frame && !irq_flags);
    assert(!writer_owned());
    /* Model the real scheduler's exclusive admission: another CPU may still
     * be a reader, but this caller must have released its own gate. A leaked
     * reader would deadlock here and fail the bounded subprocess timeout. */
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    assert(writer_owned() && !reader_owned());
    kernel_execution_unlock_irqrestore(flags);
    assert(!irq_flags);
    atomic_fetch_add(&schedules, 1); return &next_task;
}
void kernel_signal_force_fault(struct task *task, int sig, int code, uint64_t addr)
{ assert(task == current && sig && code && addr == fault_address && writer_owned()); atomic_fetch_add(&signals, 1); }
__attribute__((noreturn)) void bugcheck_trap(const char *reason, const struct trap_frame *frame, uint64_t address)
{ (void)reason; (void)frame; (void)address; abort(); }
__attribute__((noreturn)) void bugcheck_exception(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                                   uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; abort(); }

uint64_t mm_alloc_page(void)
{
    if (atomic_load(&fail_alloc)) return 0;
    void *p = aligned_alloc(4096, 4096);
    assert(p); memset(p, 0, 4096);
    pthread_mutex_lock(&page_mutex);
    unsigned i;
    for (i = 0; i < 512 && pages[i].refs; ++i) { }
    assert(i < 512); pages[i].phys = (uintptr_t)p; pages[i].refs = 1; ++live_pages;
    pthread_mutex_unlock(&page_mutex);
    return (uintptr_t)p;
}
void *mm_phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)phys; }
uint64_t mm_virt_to_phys(const void *p) { return (uintptr_t)p; }
void mm_retain_page(uint64_t phys)
{
    pthread_mutex_lock(&page_mutex);
    unsigned i;
    for (i = 0; i < 512 && (!pages[i].refs || pages[i].phys != phys); ++i) { }
    assert(i < 512); ++pages[i].refs;
    pthread_mutex_unlock(&page_mutex);
}
void mm_free_page(uint64_t phys)
{
    pthread_mutex_lock(&page_mutex);
    unsigned i;
    for (i = 0; i < 512 && (!pages[i].refs || pages[i].phys != phys); ++i) { }
    assert(i < 512);
    if (!--pages[i].refs) { free((void *)(uintptr_t)phys); pages[i].phys = 0; --live_pages; }
    pthread_mutex_unlock(&page_mutex);
}

/* Rendezvous inside the production map/COW path: old exclusive dispatch times
 * out safely instead of hanging; two reader transactions must reach it together. */
static pthread_mutex_t meet_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t meet_cond = PTHREAD_COND_INITIALIZER;
static bool meet_enabled;
static unsigned meet_active, meet_max;
void x86_64_invlpg(uint64_t page)
{
    (void)page;
    if (writer_owned()) mapping_owner = 2;
    else if (reader_owned()) mapping_owner = 1;
    if (reader_owned() && atomic_exchange(&notify_in_mapping, 0)) {
        atomic_store(&notify_mapping_entered, 1);
        wait_until(&notify_mapping_done, 1);
    }
    pthread_mutex_lock(&meet_mutex);
    if (meet_enabled) {
        ++meet_active;
        if (meet_active > meet_max) meet_max = meet_active;
        if (meet_active == 2) {
            meet_enabled = false;
            pthread_cond_broadcast(&meet_cond);
        } else {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 2;
            while (meet_enabled) {
                if (pthread_cond_timedwait(&meet_cond, &meet_mutex, &until) == ETIMEDOUT) {
                    meet_enabled = false;
                    pthread_cond_broadcast(&meet_cond);
                }
            }
        }
        --meet_active;
    }
    pthread_mutex_unlock(&meet_mutex);
}
static void arm_meeting(void)
{ pthread_mutex_lock(&meet_mutex); meet_enabled = true; meet_active = meet_max = 0; pthread_mutex_unlock(&meet_mutex); }

void userland_loader_lock(uint64_t *flags) { assert(writer_owned() && !reader_owned()); *flags = 0; }
void userland_loader_unlock(uint64_t flags) { (void)flags; assert(writer_owned()); }
void storage_set_io_async_context(bool enabled)
{ (void)enabled; assert(writer_owned() && !reader_owned()); }
int storage_errno(int status) { return status; }
int storage_read_node(const struct storage_node *node, uint64_t offset, void *buf,
                      uint32_t cap, uint32_t *got)
{ (void)node; (void)offset; (void)buf; (void)cap; (void)got; abort(); }
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; assert(writer_owned()); *phys = mm_alloc_page(); return *phys ? 0 : -RELIEFOS_ENOMEM; }
static int exec_fixture_read(void *context, const struct storage_node *node, uint64_t offset,
                             void *buffer, uint32_t size, uint32_t *got, uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context == &exec_fixture_pins && exec_fixture_pins && exec_fixture_denials);
    assert(writer_owned() && !reader_owned());
    assert(offset + size <= node->size && node->size <= sizeof(file_bytes));
    memcpy(buffer, file_bytes + offset, size); *got = size;
    atomic_fetch_add(&provider_reads, 1); return 0;
}

static struct task *new_task(void)
{
    struct task *task = calloc(1, sizeof(*task)); assert(task);
    task->kind = TASK_KIND_USER; task->pid = 123;
    assert(address_space_create(sched_task_as(task)));
    return task;
}
static void destroy_task(struct task *task)
{ address_space_destroy(sched_task_as(task)); free(task); }
struct fault_work { struct task *task; uint64_t address, error, cs; uint32_t cpu; struct task *result; int owner; };
static void *run_fault(void *opaque)
{
    struct fault_work *work = opaque;
    cpu = work->cpu; current = work->task; fault_address = work->address;
    if (!current->pid) current->pid = 124;
    if (!current->stack_top) {
        current->stack_top = RELIEFNT_USER_TOP - 4096;
        current->stack_low = current->stack_top - 4096;
    }
    if (!preserve_sched_state) {
        current->state = TASK_RUNNING;
        current->running_cpu = cpu;
        current->affinity_mask = 15;
        current->flags &= ~TASK_FLAG_PENDING_LOAD;
    }
    current_task[cpu] = current;
    current_pid[cpu] = current->pid + (bad_reservation ? 1u : 0u);
    if (require_no_resched) assert(!sched_current_cpu_needs_resched());
    mapping_owner = 0;
    uint64_t initial_irq_flags = irq_flags;
    struct trap_frame frame = {.cs = work->cs, .error = work->error,
        .ss = RELIEFNT_USER_DS, .rip = bad_frame ? 0 : RELIEFNT_USER_BASE + 0x1000,
        .rsp = RELIEFNT_USER_TOP - 8192, .rflags = 1ULL << 9, .rax = 0x51a7};
    live_frame = &frame;
    work->result = page_fault_dispatch(&frame); work->owner = mapping_owner;
    live_frame = NULL;
    assert(irq_flags == initial_irq_flags); return NULL;
}
static void run_pair(struct fault_work *a, struct fault_work *b)
{
    pthread_t ta, tb; arm_meeting();
    assert(!pthread_create(&ta, NULL, run_fault, a));
    assert(!pthread_create(&tb, NULL, run_fault, b));
    assert(!pthread_join(ta, NULL)); assert(!pthread_join(tb, NULL));
}

static void test_cow_overlap(void)
{
    const uint64_t va = RELIEFNT_USER_BASE + 0x4000;
    struct task *parent = new_task(), *child = calloc(1, sizeof(*child)); assert(child);
    parent->vmas[0] = (struct task_vma){.used=1, .start=va, .end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE, .flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t original = mm_alloc_page(); *(unsigned char *)(uintptr_t)original = 0x5a;
    assert(address_space_map_user_page(sched_task_as(parent), va, original,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    child->kind = TASK_KIND_USER; child->vmas[0] = parent->vmas[0];
    assert(address_space_clone_cow(sched_task_as(parent), sched_task_as(child)));
    assert(address_space_user_page_phys(sched_task_as(child), va) == original);
    require_no_resched=true;
    struct fault_work a={parent,va,7,0x23,1,NULL,0}, b={child,va,7,0x23,2,NULL,0};
    run_pair(&a,&b);
    uint64_t pa=address_space_user_page_phys(sched_task_as(parent),va);
    uint64_t pb=address_space_user_page_phys(sched_task_as(child),va);
    assert(pa && pb && pa != pb && pa != original && pb != original);
    assert(*(unsigned char *)(uintptr_t)pa == 0x5a && *(unsigned char *)(uintptr_t)pb == 0x5a);
    *(unsigned char *)(uintptr_t)pa=0x73; assert(*(unsigned char *)(uintptr_t)pb==0x5a);
    assert(address_space_user_page_writable(sched_task_as(parent),va));
    assert(address_space_user_page_writable(sched_task_as(child),va));
    assert(meet_max==2 && a.owner==1 && b.owner==1);
    if (a.result!=&next_task || b.result!=&next_task || atomic_load(&schedules)!=2)
        fprintf(stderr,"FAIL overlapping COW readers skipped no-pending normal checkpoints\n");
    assert(a.result==&next_task && b.result==&next_task && atomic_load(&schedules)==2);
    destroy_task(child); destroy_task(parent); assert(!live_pages);
}

static void test_cow_policy_and_fallback(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x8000;
    struct task *parent=new_task(), *child=calloc(1,sizeof(*child)); assert(child);
    parent->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t original=mm_alloc_page(); *(unsigned char *)(uintptr_t)original=0x4c;
    assert(address_space_map_user_page(sched_task_as(parent),va,original,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    child->kind=TASK_KIND_USER; child->vmas[0]=parent->vmas[0];
    assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
    struct fault_work w={child,va,15,0x23,0,NULL,0};
    unsigned before=atomic_load(&signals); run_fault(&w); /* reserved bit */
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+1);
    w.error=23; run_fault(&w); /* instruction-fetch plus write */
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+2);
    assert(address_space_user_page_phys(sched_task_as(child),va)==original);

    child->vmas[0].prot=LINUX_PROT_READ;
    assert(address_space_protect_user_page(sched_task_as(child),va,RELIEFNT_PAGE_NOEXEC));
    w.error=7; run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+3);
    child->vmas[0].prot=LINUX_PROT_NONE;
    assert(address_space_protect_user_page(sched_task_as(child),va,
                                           RELIEFNT_PAGE_NOEXEC|RELIEFNT_PAGE_PROTNONE));
    run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+4);
    child->vmas[0].prot=LINUX_PROT_READ|LINUX_PROT_WRITE;
    assert(address_space_protect_user_page(sched_task_as(child),va,
                                           RELIEFNT_PAGE_NOEXEC|RELIEFNT_PAGE_WRITABLE));
    assert(address_space_user_page_flags(sched_task_as(child),va)&RELIEFNT_PAGE_COW);
    atomic_store(&fail_alloc,true); run_fault(&w); atomic_store(&fail_alloc,false);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+5);
    assert(address_space_user_page_phys(sched_task_as(child),va)==original);

    /* CLONE_VM is still writer-owned and schedules after the resolved fault;
     * kernel-mode usercopy stays recursive writer-owned and resumes directly. */
    child->shared_mm=&child->address_space; run_fault(&w);
    assert(w.result==&next_task && w.owner==2);
    uint64_t child_page=address_space_user_page_phys(sched_task_as(child),va);
    assert(child_page!=original && *(unsigned char *)(uintptr_t)child_page==0x4c);
    child->shared_mm=NULL;
    w.task=parent; w.error=3; w.cs=0x08;
    cpu=0; sched_request_reschedule_current();
    unsigned schedules_before=atomic_load(&schedules);
    uint64_t outer_flags; kernel_execution_lock_irqsave(&outer_flags);
    run_fault(&w);
    assert(!w.result && w.owner==2 && writer_owned() && execution_depth[cpu]==1 &&
           atomic_load(&schedules)==schedules_before);
    kernel_execution_unlock_irqrestore(outer_flags);
    uint64_t parent_page=address_space_user_page_phys(sched_task_as(parent),va);
    assert(parent_page!=child_page && *(unsigned char *)(uintptr_t)parent_page==0x4c);
    *(unsigned char *)(uintptr_t)parent_page=0x22;
    assert(*(unsigned char *)(uintptr_t)child_page==0x4c);
    destroy_task(child); destroy_task(parent); assert(!live_pages);
}

static void test_stack_cow(void)
{
    const uint64_t va=RELIEFNT_USER_TOP-0x20000;
    struct task *parent=new_task(), *child=calloc(1,sizeof(*child)); assert(child);
    parent->stack_low=va; parent->stack_top=va+4096;
    parent->address_space.initial_stack_low=va; parent->address_space.initial_stack_top=va+4096;
    uint64_t original=mm_alloc_page(); *(unsigned char *)(uintptr_t)original=0x68;
    assert(address_space_map_user_page(sched_task_as(parent),va,original,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    child->kind=TASK_KIND_USER; child->stack_low=va; child->stack_top=va+4096;
    child->address_space.initial_stack_low=va; child->address_space.initial_stack_top=va+4096;
    assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
    require_no_resched=true;
    struct fault_work w={child,va,7,0x23,0,NULL,0}; run_fault(&w);
    uint64_t copied=address_space_user_page_phys(sched_task_as(child),va);
    assert(copied!=original && *(unsigned char *)(uintptr_t)copied==0x68);
    assert(address_space_user_page_phys(sched_task_as(parent),va)==original);
    assert(w.owner==1);
    assert(w.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(child); destroy_task(parent); assert(!live_pages);
}

static void wait_until(atomic_uint *value, unsigned target)
{
    struct timespec pause={.tv_nsec=1000000};
    for (unsigned i=0; i<3000 && atomic_load(value)<target; ++i) nanosleep(&pause,NULL);
    assert(atomic_load(value)>=target && "execution gate failed to progress");
}
static atomic_uint writer_entered, writer_release;
static atomic_bool notify_while_writer_held;
static void *blocking_writer(void *unused)
{
    (void)unused; cpu=1; uint64_t flags;
    kernel_execution_lock_irqsave(&flags); atomic_store(&writer_entered,1);
    wait_until(&writer_release,1);
    if (atomic_load(&notify_while_writer_held)) sched_notify_runnable();
    kernel_execution_unlock_irqrestore(flags); return NULL;
}
static struct fault_work run_behind_writer(struct task *task, uint64_t va, uint64_t error,
                                           bool notify)
{
    atomic_store(&polls,0); atomic_store(&writer_entered,0); atomic_store(&writer_release,0);
    atomic_store(&notify_while_writer_held,notify);
    uint64_t old_phys=address_space_user_page_phys(sched_task_as(task),va);
    uint64_t flags; cpu=0; assert(kernel_execution_try_read_lock_irqsave(&flags));
    uint32_t ticket_before=__atomic_load_n(&execution_next_ticket,__ATOMIC_SEQ_CST);
    pthread_t writer, fault;
    assert(!pthread_create(&writer,NULL,blocking_writer,NULL));
    wait_until(&polls,1);
    struct fault_work work={task,va,error,0x23,2,NULL,0};
    assert(!pthread_create(&fault,NULL,run_fault,&work));
    for (unsigned i=0; i<3000 && __atomic_load_n(&execution_next_ticket,__ATOMIC_SEQ_CST)<ticket_before+2; ++i) {
        struct timespec delay={.tv_nsec=1000000}; nanosleep(&delay,NULL);
    }
    assert(__atomic_load_n(&execution_next_ticket,__ATOMIC_SEQ_CST)>=ticket_before+2);
    kernel_execution_read_unlock_irqrestore(flags);
    wait_until(&writer_entered,1);
    assert(address_space_user_page_phys(sched_task_as(task),va)==old_phys);
    atomic_store(&writer_release,1); assert(!pthread_join(writer,NULL)); assert(!pthread_join(fault,NULL));
    return work;
}
static void test_writer_fallback(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x6000;
    struct task *task=new_task();
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    require_no_resched=true;
    struct fault_work work=run_behind_writer(task,va,6,false);
    assert(address_space_user_page_writable(sched_task_as(task),va));
    assert(work.owner==2);
    if (work.result!=&next_task) fprintf(stderr,"FAIL queued demand-zero skipped normal checkpoint\n");
    assert(work.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(task); assert(!live_pages);
}
static void test_writer_cow_fallback(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0xa000;
    struct task *parent=new_task(), *child=calloc(1,sizeof(*child)); assert(child);
    parent->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t original=mm_alloc_page(); *(unsigned char *)(uintptr_t)original=0x4e;
    assert(address_space_map_user_page(sched_task_as(parent),va,original,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    child->kind=TASK_KIND_USER; child->vmas[0]=parent->vmas[0];
    assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
    require_no_resched=true;
    struct fault_work work=run_behind_writer(child,va,7,false);
    uint64_t private_page=address_space_user_page_phys(sched_task_as(child),va);
    assert(private_page!=original && *(unsigned char *)(uintptr_t)private_page==0x4e);
    assert(address_space_user_page_phys(sched_task_as(parent),va)==original);
    assert(work.owner==2);
    if (work.result!=&next_task) fprintf(stderr,"FAIL queued COW skipped normal checkpoint\n");
    assert(work.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(child); destroy_task(parent); assert(!live_pages);
}

static void test_policy_and_shared_fallback(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x10000;
    struct task *task=new_task();
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    struct fault_work w={task,va,6,0x23,0,NULL,0};
    unsigned before=atomic_load(&signals); run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+1);
    task->vmas[0].prot=LINUX_PROT_NONE; w.error=4; run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+2);
    task->vmas[0].prot=LINUX_PROT_READ; w.error=20; run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+3);
    w.error=12; run_fault(&w);
    assert(w.result==&next_task && !w.owner && atomic_load(&signals)==before+4);
    assert(!address_space_user_page_phys(sched_task_as(task),va));
    task->shared_mm=&task->address_space; w.error=4; run_fault(&w);
    assert(w.result==&next_task && w.owner==2);
    task->shared_mm=NULL;
    const uint64_t second=va+4096;
    task->vmas[0].end=second+4096; task->vmas[0].prot=LINUX_PROT_READ|LINUX_PROT_WRITE;
    w.address=second; w.error=2; w.cs=0x08; run_fault(&w);
    assert(!w.result && w.owner==2 && address_space_user_page_writable(sched_task_as(task),second));
    destroy_task(task); assert(!live_pages);
}

static struct task *file_task(struct vfs_exec_source *source, struct storage_node node,
                              uint64_t va, uint64_t offset, uint64_t limit, uint32_t flags, uint64_t prot)
{
    struct task *task=new_task();
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,.prot=prot,
        .flags=flags,.file_node=node,.file_offset=offset,.file_limit=limit,.exec_source=source};
    return task;
}
static uint64_t warm_file(struct vfs_exec_source *source, struct storage_node node, uint64_t offset)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x20000;
    struct task *task=file_task(source,node,va,offset,node.size,
        TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE,
        LINUX_PROT_READ|LINUX_PROT_EXEC);
    uint64_t flags; cpu=0; kernel_execution_lock_irqsave(&flags);
    assert(!task_map_file_vma_page(task,&task->vmas[0],va));
    uint64_t page=address_space_unmap_user_page(sched_task_as(task),va); assert(page);
    kernel_execution_unlock_irqrestore(flags); destroy_task(task); return page;
}
static void test_file_cache(void)
{
    for (unsigned i=0;i<sizeof(file_bytes);++i) file_bytes[i]=(unsigned char)(0x10+i/4096);
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=71,.volume_id=3,.size=8192+512};
    struct vfs_exec_source *source=exec_fixture_open(&node);
    page_cache_init();
    uint64_t warm_a=warm_file(source,node,4096), warm_b=warm_file(source,node,8192);
    unsigned reads=atomic_load(&provider_reads), pins=exec_fixture_pins, denies=exec_fixture_denials;
    const uint32_t valid=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE;
    const uint64_t va=RELIEFNT_USER_BASE+0x30000;
    struct task *a=file_task(source,node,va,4096,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC);
    struct task *b=file_task(source,node,va,8192,node.size,valid,LINUX_PROT_READ);
    assert(address_space_prepare_user_range(sched_task_as(a),va,va+4096));
    assert(address_space_prepare_user_range(sched_task_as(b),va,va+4096));
    require_no_resched=true;
    struct fault_work wa={a,va,20,0x23,1,NULL,0}, wb={b,va,4,0x23,2,NULL,0};
    run_pair(&wa,&wb);
    uint64_t pa=address_space_user_page_phys(sched_task_as(a),va);
    uint64_t pb=address_space_user_page_phys(sched_task_as(b),va);
    assert(pa && pb && *(unsigned char *)(uintptr_t)pa==0x11 && *(unsigned char *)(uintptr_t)pb==0x12);
    assert(((unsigned char *)(uintptr_t)pb)[511]==0x12 && ((unsigned char *)(uintptr_t)pb)[512]==0);
    assert(!address_space_user_page_writable(sched_task_as(a),va));
    assert(address_space_user_page_flags(sched_task_as(b),va)&RELIEFNT_PAGE_NOEXEC);
    assert(atomic_load(&provider_reads)==reads && exec_fixture_pins==pins && exec_fixture_denials==denies);
    assert(meet_max==2 && wa.owner==1 && wb.owner==1);
    if (wa.result!=&next_task || wb.result!=&next_task || atomic_load(&schedules)!=2)
        fprintf(stderr,"FAIL overlapping cache readers skipped no-pending normal checkpoints\n");
    bool overlap_ok=wa.result==&next_task && wb.result==&next_task && atomic_load(&schedules)==2;
    destroy_task(a); destroy_task(b);

    struct task *queued=file_task(source,node,va,4096,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC);
    assert(address_space_prepare_user_range(sched_task_as(queued),va,va+4096));
    struct fault_work queued_result=run_behind_writer(queued,va,20,false);
    uint64_t queued_phys=address_space_user_page_phys(sched_task_as(queued),va);
    assert(queued_phys && *(unsigned char *)(uintptr_t)queued_phys==0x11);
    assert(atomic_load(&provider_reads)==reads);
    assert(queued_result.owner==2);
    bool queued_ok=queued_result.result==&next_task &&
        atomic_load(&schedules)==3;
    if (!queued_ok) fprintf(stderr,"FAIL queued cache hit skipped normal checkpoint\n");
    destroy_task(queued);

    /* Miss, shared, writable, device, segment tail, EOF and tmpfs each stay
     * on the writer/provider path; the last case checks cache-ref rollback. */
    struct { uint64_t offset,limit; uint32_t flags; uint64_t prot; bool tmpfs, no_source; } cases[]={
        {0,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC,false,false},
        {4096,node.size,valid|TASK_VMA_FLAG_SHARED,LINUX_PROT_READ|LINUX_PROT_EXEC,false,false},
        {4096,node.size,valid,LINUX_PROT_READ|LINUX_PROT_WRITE,false,false},
        {4096,node.size,valid|TASK_VMA_FLAG_DEVICE,LINUX_PROT_READ|LINUX_PROT_EXEC,false,false},
        {8192,8192+256,valid,LINUX_PROT_READ|LINUX_PROT_EXEC,false,false},
        {12288,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC,false,false},
        {4096,node.size,valid|TASK_VMA_FLAG_MMAP,LINUX_PROT_READ,true,false},
        {4096,node.size,valid|TASK_VMA_FLAG_MMAP,LINUX_PROT_READ|LINUX_PROT_EXEC,false,true},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        struct storage_node item=node; if (cases[i].tmpfs) item.flags|=STORAGE_NODE_FLAG_TMPFS;
        struct task *t=file_task(cases[i].no_source ? NULL : source,item,va,cases[i].offset,cases[i].limit,cases[i].flags,cases[i].prot);
        struct fault_work w={t,va,4,0x23,0,NULL,0}; run_fault(&w);
        assert(w.result==&next_task && w.owner==2);
        uint64_t phys=address_space_user_page_phys(sched_task_as(t),va); assert(phys);
        if (i==4) assert(((unsigned char *)(uintptr_t)phys)[255]==0x12 && ((unsigned char *)(uintptr_t)phys)[256]==0);
        if (i==5) assert(((unsigned char *)(uintptr_t)phys)[0]==0);
        destroy_task(t);
    }
    /* An unrecognized hardware error bit takes the old writer path even for
     * a resident page. Denied accesses retain their original signal behavior. */
    struct task *unknown=file_task(source,node,va,4096,node.size,valid,LINUX_PROT_READ);
    struct fault_work wu={unknown,va,0x24,0x23,0,NULL,0}; run_fault(&wu);
    assert(wu.result==&next_task && wu.owner==2);
    destroy_task(unknown);
    const struct { uint64_t prot, error; } denied[]={
        {LINUX_PROT_NONE,4}, {LINUX_PROT_READ,20},
        {LINUX_PROT_READ|LINUX_PROT_EXEC,6},
    };
    unsigned signals_before=atomic_load(&signals);
    for (unsigned i=0;i<sizeof(denied)/sizeof(denied[0]);++i) {
        struct task *t=file_task(source,node,va,4096,node.size,valid,denied[i].prot);
        struct fault_work w={t,va,denied[i].error,0x23,0,NULL,0}; run_fault(&w);
        assert(w.result==&next_task && !w.owner &&
               atomic_load(&signals)==signals_before+i+1);
        assert(!address_space_user_page_phys(sched_task_as(t),va)); destroy_task(t);
    }
    struct task *failed=file_task(source,node,va,4096,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC);
    atomic_store(&fail_alloc,true);
    struct fault_work wf={failed,va,4,0x23,0,NULL,0}; run_fault(&wf);
    atomic_store(&fail_alloc,false);
    assert(wf.result==&next_task && !address_space_user_page_phys(sched_task_as(failed),va));
    destroy_task(failed);
    page_cache_release(warm_a); page_cache_release(warm_b);
    page_cache_invalidate_node(&node);
    uint64_t leaked=0; assert(page_cache_lookup(&node,4096,&leaked)!=0);
    assert(page_cache_lookup(&node,8192,&leaked)!=0);
    vfs_exec_source_put(source);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials && !live_pages);
    assert(overlap_ok && queued_ok);
}

/* A real remote scheduler notification arrives while this CPU is inside the
 * production reader-owned mapping path, before its user-return decision. */
static void *notify_during_fault(void *unused)
{
    (void)unused; cpu=3;
    wait_until(&notify_mapping_entered,1);
    sched_notify_runnable();
    atomic_store(&notify_mapping_done,1);
    return NULL;
}
static void run_with_remote_notification(struct fault_work *work)
{
    atomic_store(&notify_mapping_entered,0); atomic_store(&notify_mapping_done,0);
    atomic_store(&notify_in_mapping,1);
    pthread_t notifier; assert(!pthread_create(&notifier,NULL,notify_during_fault,NULL));
    run_fault(work);
    assert(!pthread_join(notifier,NULL));
    assert(!atomic_load(&notify_in_mapping));
    assert(sched_current_cpu_needs_resched());
}
static void test_reader_cow_reschedule(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x4000;
    struct task *parent=new_task(), *child=calloc(1,sizeof(*child)); assert(child);
    parent->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t original=mm_alloc_page(); *(unsigned char *)(uintptr_t)original=0x5a;
    assert(address_space_map_user_page(sched_task_as(parent),va,original,
                                       RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    child->kind=TASK_KIND_USER; child->vmas[0]=parent->vmas[0];
    assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
    struct fault_work work={child,va,7,0x23,0,NULL,0};
    run_with_remote_notification(&work);
    uint64_t copied=address_space_user_page_phys(sched_task_as(child),va);
    assert(copied!=original && *(unsigned char *)(uintptr_t)copied==0x5a && work.owner==1);
    if (work.result!=&next_task || atomic_load(&schedules)!=1)
        fprintf(stderr,"FAIL reader COW returned directly despite remote reschedule\n");
    assert(work.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(child); destroy_task(parent); assert(!live_pages);
}
static void test_reader_cache_reschedule(void)
{
    memset(file_bytes,0x2a,sizeof(file_bytes));
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=71,.volume_id=3,.size=4096};
    struct vfs_exec_source *source=exec_fixture_open(&node);
    page_cache_init(); uint64_t warm=warm_file(source,node,0);
    unsigned reads=atomic_load(&provider_reads);
    const uint64_t va=RELIEFNT_USER_BASE+0x30000;
    const uint32_t valid=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_SHARED_FILE;
    struct task *task=file_task(source,node,va,0,node.size,valid,LINUX_PROT_READ|LINUX_PROT_EXEC);
    assert(address_space_prepare_user_range(sched_task_as(task),va,va+4096));
    struct fault_work work={task,va,20,0x23,0,NULL,0};
    run_with_remote_notification(&work);
    uint64_t phys=address_space_user_page_phys(sched_task_as(task),va);
    assert(phys && *(unsigned char *)(uintptr_t)phys==0x2a && work.owner==1);
    assert(atomic_load(&provider_reads)==reads);
    if (work.result!=&next_task || atomic_load(&schedules)!=1)
        fprintf(stderr,"FAIL reader executable cache hit returned directly despite remote reschedule\n");
    assert(work.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(task); page_cache_release(warm); page_cache_invalidate_node(&node);
    vfs_exec_source_put(source);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials && !live_pages);
}
static void test_writer_reschedule(void)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x6000;
    struct task *task=new_task();
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    struct fault_work work=run_behind_writer(task,va,6,true);
    cpu=2; assert(sched_current_cpu_needs_resched());
    assert(address_space_user_page_writable(sched_task_as(task),va) && work.owner==2);
    if (work.result!=&next_task || atomic_load(&schedules)!=1)
        fprintf(stderr,"FAIL queued private fault returned directly despite remote reschedule\n");
    assert(work.result==&next_task && atomic_load(&schedules)==1);
    destroy_task(task); assert(!live_pages);
}

/* Each scenario runs in its own process so no previous pending reschedule can
 * mask the particular production predicate being checked here. */
static void test_return_predicate(const char *which)
{
    const uint64_t va=RELIEFNT_USER_BASE+0x10000, signal=1ULL<<5;
    struct task *task=new_task();
    task->vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    task->state=TASK_RUNNING; task->running_cpu=0; task->affinity_mask=15;
    preserve_sched_state=true;
    bool safe=false;
    if (!strcmp(which,"safe")) safe=true;
    else if (!strcmp(which,"thread-signal")) task->pending_signals=signal;
    else if (!strcmp(which,"process-signal")) task->process_pending_signals=signal;
    else if (!strcmp(which,"blocked-signal")) {
        task->pending_signals=signal; task->blocked_signals=signal; safe=true;
    } else if (!strcmp(which,"stopped")) task->state=TASK_STOPPED;
    else if (!strcmp(which,"exited")) task->state=TASK_EXITED;
    else if (!strcmp(which,"affinity")) task->affinity_mask=2;
    else if (!strcmp(which,"bad-frame")) bad_frame=true;
    else if (!strcmp(which,"bad-reservation")) bad_reservation=true;
    else if (!strcmp(which,"pending-load")) task->flags|=TASK_FLAG_PENDING_LOAD;
    else abort();
    cpu=0; assert(!sched_current_cpu_needs_resched());
    struct fault_work work={task,va,4,0x23,0,NULL,0}; run_fault(&work);
    assert(!sched_current_cpu_needs_resched());
    assert(address_space_user_page_phys(sched_task_as(task),va) && work.owner==1);
    if (safe) {
        assert(!work.result && !atomic_load(&schedules));
        assert(task->frame.rax==0x51a7 && task->frame.rip==RELIEFNT_USER_BASE+0x1000);
    } else {
        if (work.result!=&next_task || atomic_load(&schedules)!=1)
            fprintf(stderr,"FAIL reader fault bypassed normal user-return predicate: %s\n",which);
        assert(work.result==&next_task && atomic_load(&schedules)==1);
    }
    destroy_task(task); assert(!live_pages);
}

int main(int argc, char **argv)
{
    assert(argc==2); nx_enabled=true;
    if (!strcmp(argv[1],"cow")) test_cow_overlap();
    else if (!strcmp(argv[1],"cow-policy")) test_cow_policy_and_fallback();
    else if (!strcmp(argv[1],"stack-cow")) test_stack_cow();
    else if (!strcmp(argv[1],"writer")) test_writer_fallback();
    else if (!strcmp(argv[1],"writer-cow")) test_writer_cow_fallback();
    else if (!strcmp(argv[1],"policy")) test_policy_and_shared_fallback();
    else if (!strcmp(argv[1],"cache")) test_file_cache();
    else if (!strcmp(argv[1],"resched-reader-cow")) test_reader_cow_reschedule();
    else if (!strcmp(argv[1],"resched-reader-cache")) test_reader_cache_reschedule();
    else if (!strcmp(argv[1],"resched-writer")) test_writer_reschedule();
    else if (!strncmp(argv[1],"return-",7)) test_return_predicate(argv[1]+7);
    else abort();
    printf("PASS real private fault dispatch: %s\n",argv[1]); return 0;
}

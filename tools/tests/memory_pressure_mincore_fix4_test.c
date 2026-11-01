/* SPDX-License-Identifier: Apache-2.0 */
/* The real mincore, syscall-frame body, execution writer, reclaim cursor,
 * timed wait, PTE and swap-slot implementations. Physical-page supply,
 * wakeup and swap transport are bounded host models. The host output buffer
 * and simulated physical frame are distinct; byte restoration is checked
 * separately from the actual C-store results. */
#define PRESSURE_FIX4_VECTOR_FIXTURE 1
#define PRESSURE_FIX5_FIXTURE_ENTRY pressure_fix5_unused_main
#include "memory_pressure_fix5_test.c"
#undef LINUX_PROT_READ
#undef LINUX_PROT_WRITE
#undef LINUX_PROT_EXEC
#undef LINUX_MAP_SUPPORTED
#include "../../kernel/syscall/internal.h"
#include <reliefnt/graphics_service.h>
#include <reliefnt/lock_profile.h>
#include <reliefnt/module.h>
#include <reliefnt/usb.h>

/* The runner links the real usercopy implementation; host output bytes and
 * modeled physical frames remain distinct, as described above. */

static bool inject_read_eio;
#if defined(PRESSURE_REGULAR_IO_FIXTURE) || defined(PRESSURE_OPEN_FIXTURE)
static bool pressure_regular_contend_writes;
#endif
static unsigned injected_read_eio;
int __real_swap_backend_page_io(struct swap_area *,uint32_t,uint64_t,bool);
int __wrap_swap_backend_page_io(struct swap_area *area,uint32_t page,uint64_t phys,bool write)
{
#if defined(PRESSURE_REGULAR_IO_FIXTURE) || defined(PRESSURE_OPEN_FIXTURE)
    if (write && pressure_regular_contend_writes) {
        assert(writer_owned() && !reader_owned() && !user_page_fault_lock.state);
        return SWAP_STORAGE_CONTENDED;
    }
#endif
    if (!write && inject_read_eio) {
        assert(writer_owned() && !reader_owned() && !user_page_fault_lock.state);
        ++injected_read_eio;
        return -EIO;
    }
    return __real_swap_backend_page_io(area,page,phys,write);
}

static bool syscall_trace_task(const struct task *task) { (void)task; return false; }
static int task_begin_syscall_io(struct task *task, uint64_t nr, int32_t fd, uint64_t arg)
{
    assert(task == current && writer_owned()); (void)nr; (void)fd; (void)arg;
#ifdef PRESSURE_REGULAR_IO_FIXTURE
    if (nr == LINUX_SYS_READ) {
        assert(fd == 3 && pressure_regular_fixture_file);
        task->syscall_file = pressure_regular_fixture_file;
        task->syscall_fd = fd; task->syscall_file_number = (uint32_t)nr;
    }
#endif
    return 0;
}
static int syscall_eagain_is_nonblocking_device(struct task *task, uint64_t nr, uint64_t fd)
{ (void)task; (void)nr; (void)fd; return 0; }
void net_tcp_maintenance(void) { assert(writer_owned()); }
uint32_t graphics_service_process_retirements(uint32_t n)
{ assert(writer_owned() && n == 2); return 0; }
void driver_manager_process_audio_faults(void) { assert(writer_owned()); }
void usb_process_work(void) { assert(writer_owned()); }
void input_process_pending(void) { assert(writer_owned()); }
void task_socket_stage_enter(struct task *t,const struct trap_frame *f)
{ assert(t == current && f && writer_owned()); }
void task_transfer_enter(struct task *t,const struct trap_frame *f)
{ assert(t == current && f && writer_owned()); }
void task_vector_enter(struct task *t,const struct trap_frame *f)
{ assert(t == current && f && writer_owned()); }
struct task *sched_exec_staged(const struct task *task) { (void)task; return NULL; }
void userland_exec_cancel(struct task *task) { (void)task; abort(); }
void sched_exit(uint32_t pid, uint64_t status) { (void)pid; (void)status; abort(); }
#ifdef PRESSURE_REGULAR_IO_FIXTURE
void task_sysv_sem_cancel(struct task *task) { assert(task == current); }
void task_sysv_msg_cancel(struct task *task) { assert(task == current); }
void task_fifo_cancel(struct task *task) { assert(task == current); }
#else
void task_sysv_sem_cancel(struct task *task) { (void)task; abort(); }
void task_sysv_msg_cancel(struct task *task) { (void)task; abort(); }
void task_fifo_cancel(struct task *task) { (void)task; abort(); }
#endif
void storage_drain_task_io(uint32_t pid) { assert(pid == current->pid && writer_owned()); }
void storage_release_task_io(uint32_t pid) { assert(pid == current->pid && writer_owned()); }
void arch_set_user_fs(uint64_t fs) { (void)fs; assert(writer_owned()); }
#ifndef PRESSURE_OPEN_FIXTURE
void vfs_walk_cancel(struct vfs_walk **walk) { assert(walk && !*walk); }
#endif
#ifdef PRESSURE_REGULAR_IO_FIXTURE
extern void memory_wait_cancel(struct task *) __attribute__((weak));
#include "regular_release.inc"
#else
void task_release_syscall_file(struct task *task)
{ assert(task == current && writer_owned()); task_lookup_scope_cancel(task); }
#endif
void task_process_vm_scope_release(void *state) { (void)state; abort(); }
void sched_task_identity_put(struct task_identity *identity) { (void)identity; abort(); }
int64_t kernel_signal_rt_sigreturn(struct task *t,struct trap_frame *f)
{ (void)t; (void)f; abort(); }
int64_t syscall_clone_prepared(const struct trap_frame *f,uint64_t a,uint64_t b,
                              uint64_t c,uint64_t d,uint64_t e)
{ (void)f;(void)a;(void)b;(void)c;(void)d;(void)e;abort(); }
int64_t syscall_clone3(const struct trap_frame *f,uint64_t a,uint64_t b)
{ (void)f;(void)a;(void)b;abort(); }
int64_t sched_fork_current(const struct trap_frame *f) { (void)f; abort(); }
int64_t sched_vfork_current(const struct trap_frame *f) { (void)f; abort(); }
#ifdef PRESSURE_OPEN_FIXTURE
static int64_t pressure_fixture_open(struct task *task,uint64_t nr,uint64_t a,uint64_t b);
#endif
static int64_t mincore_fixture_dispatch(struct task *task,uint64_t nr,uint64_t a,
    uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{
    assert(task == current && writer_owned());
    (void)d;(void)e;(void)f;
#ifdef PRESSURE_OPEN_FIXTURE
    if (nr==LINUX_SYS_OPEN || nr==LINUX_SYS_OPENAT)
        return pressure_fixture_open(task,nr,a,b);
#endif
#ifdef PRESSURE_REGULAR_IO_FIXTURE
    if (nr == LINUX_SYS_READ) {
        assert(task->syscall_file && task->syscall_fd == (int32_t)a);
        return syscall_regular_io(task,task->syscall_file,b,c,0,false,false);
    }
#endif
    assert(nr == LINUX_SYS_MINCORE);
    return syscall_mm_mincore(a,b,c);
}
#ifndef PRESSURE_REGULAR_IO_FIXTURE
int64_t syscall_regular_read_reclaim(struct task *task,const struct trap_frame *frame)
{ (void)task;(void)frame;abort(); }
#endif
#undef RELIEFOS_SYSCALL_FRAME_DISPATCH
#define RELIEFOS_SYSCALL_FRAME_DISPATCH mincore_fixture_dispatch
#include "mincore_syscall_entry.inc"

/* One retry crosses the real frame boundary; waking the modeled timed wait
 * does not retain a borrowed PTE, task pointer or execution lock. */
static void invoke_one(struct trap_frame *frame, unsigned *attempts)
{
    ++pressure_fixture_tick;
    current->state=TASK_RUNNING; current->running_cpu=0;
    if(current->waiting_queue) kernel_wait_queue_remove(current->waiting_queue,current);
    frame->rip=0x400002; frame->rax=LINUX_SYS_MINCORE;
    (void)syscall_dispatch_frame(frame);
    assert(!writer_owned() && !reader_owned() && !user_page_fault_lock.state);
    assert(++*attempts<1800);
}
static int64_t invoke(struct trap_frame *frame, unsigned *attempts)
{
    do { invoke_one(frame,attempts); } while(frame->rax==LINUX_SYS_MINCORE);
    return (int64_t)frame->rax;
}

static void swapped_output_shortage(void)
{
    struct swap_area *area = setup_area();
    struct task *task = new_task();
    const uint64_t query = 0x30000000;
    const uint64_t vector = (uintptr_t)mincore_result + 4094;
    const uint64_t page = vector & ~4095ULL;
    assert((vector & 4095u)==4094 && vector >= RELIEFNT_USER_BASE && vector < RELIEFNT_USER_TOP);
    task->vmas[0] = (struct task_vma){.used=1,.start=page,.end=page+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    create_victims(task, query, 8);
    uint64_t physical = mm_alloc_page(), physical2 = mm_alloc_page(); assert(physical && physical2);
    memset(mm_phys_to_virt(physical), 0xa5, 4096);
    memset(mm_phys_to_virt(physical2), 0xb6, 4096);
    memset(mincore_result, 0xa5, sizeof(mincore_result));
    assert(address_space_map_user_page(sched_task_as(task),page,physical,
                                      RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(address_space_map_user_page(sched_task_as(task),page+4096,physical2,
                                      RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    prepare_task(task);
    uint64_t irq; kernel_execution_lock_irqsave(&irq);
    assert(swap_try_evict(sched_task_as(task),page)==1);
    assert(swap_try_evict(sched_task_as(task),page+4096)==1);
    uint64_t token = mm_swap_record_lookup(sched_task_as(task),page); assert(token);
    assert(mm_swap_record_lookup(sched_task_as(task),page+4096));
    kernel_execution_unlock_irqrestore(irq);
    assert(paging_user_entry_kind(sched_task_as(task),page)==PAGING_USER_ENTRY_SWAP);
    pressure_baseline_pages = live_pages; quota_enforced = true;
    struct trap_frame frame = {.rax=LINUX_SYS_MINCORE,.rdi=query,.rsi=16384,.rdx=vector,
        .rip=0x400002,.rsp=RELIEFNT_USER_TOP-8192,.cs=0x23,.ss=0x1b,.rflags=1ULL<<9};
    unsigned attempts=0; int64_t result=invoke(&frame,&attempts);
    if (result) fprintf(stderr,"expected reclaim and successful mincore; result=%lld attempts=%u token=%llx reads=%u writes=%u\n",
        (long long)result,attempts,(unsigned long long)token,gate_reads,gate_writes);
    assert(result==0 && attempts>1 && gate_reads>=2 && gate_writes>2);
    for(unsigned i=0;i<4;++i) assert(((uint8_t *)(uintptr_t)vector)[i]<=1);
    assert(!task->waiting_queue);
    quota_enforced=false;
    kernel_execution_lock_irqsave(&irq);
    for(unsigned n=0;n<2;++n)
        if(paging_user_entry_kind(sched_task_as(task),page+n*4096)==PAGING_USER_ENTRY_SWAP)
            assert(swap_resolve_fault(task,page+n*4096,6)==1);
    kernel_execution_unlock_irqrestore(irq);
    physical=address_space_user_page_phys(sched_task_as(task),page); assert(physical);
    physical2=address_space_user_page_phys(sched_task_as(task),page+4096); assert(physical2);
    for(unsigned i=0;i<4096;++i) {
        assert(((uint8_t *)mm_phys_to_virt(physical))[i]==0xa5);
        assert(((uint8_t *)mm_phys_to_virt(physical2))[i]==0xb6);
    }
    assert(!mm_swap_record_lookup(sched_task_as(task),page));
    assert(!mm_swap_record_lookup(sched_task_as(task),page+4096));
    restore_victims(task,query,8);
    quota_enforced=false; destroy_task(task);
    assert(!live_pages && !swap_area_unpublish(area)); swap_area_destroy(area);
    printf("PASS mincore valid swapped output: %u frame entries, real eviction/page-in and slots\n",attempts);
}

/* Exercise an untouched writable output under a genuinely exhausted page
 * quota.  The fake PTEs, rather than the separately backed host buffer, decide
 * whether the result vector is resident. */
static void lazy_output_shortage(void)
{
    struct swap_area *area=setup_area();
    struct task *task=new_task();
    const uint64_t query=0x30000000;
    const uint64_t page=(uintptr_t)mincore_result;
    const uint64_t vector=page+4094;
    assert(!(page&4095) && vector<RELIEFNT_USER_TOP);
    task->vmas[0]=(struct task_vma){.used=1,.start=page,.end=page+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    task->vmas[2]=(struct task_vma){.used=1,.start=page+8192,.end=page+12288,
        .prot=LINUX_PROT_READ,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    create_victims(task,query,8);
    assert(address_space_prepare_user_range(sched_task_as(task),page,page+12288));
    prepare_task(task);
    struct trap_frame frame={.rax=LINUX_SYS_MINCORE,.rdi=query,.rsi=16384,.rdx=page+12288,
        .rip=0x400002,.rsp=RELIEFNT_USER_TOP-8192,.cs=0x23,.ss=0x1b,.rflags=1ULL<<9};
    unsigned attempts=0;
    assert(invoke(&frame,&attempts)==-RELIEFOS_EFAULT && attempts==1);
    frame.rdx=page+8192;attempts=0;
    assert(invoke(&frame,&attempts)==-RELIEFOS_EFAULT && attempts==1);
    frame.rdi=query+8*4096;frame.rdx=page;attempts=0;
    assert(invoke(&frame,&attempts)==-RELIEFOS_ENOMEM && attempts==1);
    assert(paging_user_entry_kind(sched_task_as(task),page)==PAGING_USER_ENTRY_EMPTY);
    assert(paging_user_entry_kind(sched_task_as(task),page+4096)==PAGING_USER_ENTRY_EMPTY);
    memset(mincore_result,0xa5,sizeof(mincore_result));
    pressure_baseline_pages=live_pages;quota_enforced=true;
    unsigned writes_before=gate_writes;
    frame.rdi=query;frame.rdx=vector;attempts=0;
    int64_t result=invoke(&frame,&attempts);
    if(result)fprintf(stderr,"lazy output: result=%lld attempts=%u writes=%u\n",
                      (long long)result,attempts,gate_writes-writes_before);
    assert(!result && attempts>1 && gate_writes>writes_before);
    for(unsigned i=0;i<4;++i)assert(((uint8_t *)(uintptr_t)vector)[i]<=1);
    assert(!task->waiting_queue);
    restore_victims(task,query,8);
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    for(unsigned n=0;n<2;++n)
        if(paging_user_entry_kind(sched_task_as(task),page+n*4096)==PAGING_USER_ENTRY_SWAP)
            assert(swap_resolve_fault(task,page+n*4096,6)==1);
    kernel_execution_unlock_irqrestore(irq);
    destroy_task(task);assert(!live_pages && !swap_area_unpublish(area));swap_area_destroy(area);
    printf("PASS mincore untouched output: %u entries, eviction and cross-page copy; invalid and read-only outputs rejected\n",
           attempts);
}

struct terminal_case {
    struct swap_area *area;
    struct task *task;
    uint64_t page, token, query;
    unsigned victims;
};

/* Each control starts with a real valid swapped output and distinct query VMA. */
static struct terminal_case make_terminal_case(unsigned victims)
{
    struct terminal_case c={.area=setup_area(),.task=new_task(),
                            .page=(uintptr_t)mincore_result,.query=0x30000000,.victims=victims};
    assert(!(c.page&4095));
    c.task->vmas[0]=(struct task_vma){.used=1,.start=c.page,.end=c.page+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    if(victims) create_victims(c.task,c.query,victims);
    else c.task->vmas[1]=(struct task_vma){.used=1,.start=c.query,.end=c.query+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    uint64_t phys=mm_alloc_page();assert(phys);memset(mm_phys_to_virt(phys),0x7d,4096);
    assert(address_space_map_user_page(sched_task_as(c.task),c.page,phys,
                                      RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    prepare_task(c.task);uint64_t irq;kernel_execution_lock_irqsave(&irq);
    assert(swap_try_evict(sched_task_as(c.task),c.page)==1);
    c.token=mm_swap_record_lookup(sched_task_as(c.task),c.page);assert(c.token);
    kernel_execution_unlock_irqrestore(irq);
    return c;
}
static struct trap_frame case_frame(const struct terminal_case *c)
{
    return (struct trap_frame){.rax=LINUX_SYS_MINCORE,.rdi=c->query,.rsi=4096,.rdx=c->page,
        .rip=0x400002,.rsp=RELIEFNT_USER_TOP-8192,.cs=0x23,.ss=0x1b,.rflags=1ULL<<9};
}
static void finish_terminal_case(struct terminal_case *c)
{
    quota_enforced=false;atomic_store(&fail_alloc,false);inject_read_eio=false;
    c->task->vmas[0].prot=LINUX_PROT_READ|LINUX_PROT_WRITE;
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    if(paging_user_entry_kind(sched_task_as(c->task),c->page)==PAGING_USER_ENTRY_SWAP)
        assert(swap_resolve_fault(c->task,c->page,6)==1);
    uint64_t phys=address_space_user_page_phys(sched_task_as(c->task),c->page);assert(phys);
    for(unsigned i=0;i<4096;++i)assert(((uint8_t *)mm_phys_to_virt(phys))[i]==0x7d);
    kernel_execution_unlock_irqrestore(irq);
    if(c->victims)restore_victims(c->task,c->query,c->victims);
    destroy_task(c->task);assert(!live_pages && !swap_area_unpublish(c->area));swap_area_destroy(c->area);
}

/* No backend error is interpreted as reclaimable shortage or loses its slot. */
static void terminal_controls(void)
{
    struct terminal_case c=make_terminal_case(4);
    struct trap_frame f=case_frame(&c);unsigned attempts=0;
    f.rdi=c.query+4*4096;assert(invoke(&f,&attempts)==-RELIEFOS_ENOMEM && attempts==1);
    attempts=0;f=case_frame(&c);f.rdx=c.page+8192;
    assert(invoke(&f,&attempts)==-RELIEFOS_EFAULT && attempts==1);
    attempts=0;f=case_frame(&c);c.task->vmas[0].prot=LINUX_PROT_READ;
    assert(invoke(&f,&attempts)==-RELIEFOS_EFAULT && attempts==1);
    c.task->vmas[0].prot=LINUX_PROT_READ|LINUX_PROT_WRITE;
    assert(mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token);
    pressure_baseline_pages=live_pages+1;quota_enforced=true;inject_read_eio=true;
    unsigned writes_before=gate_writes;attempts=0;f=case_frame(&c);
    assert(invoke(&f,&attempts)==-RELIEFOS_EFAULT && attempts==1);
    assert(injected_read_eio==1 && gate_writes==writes_before && !gate_reads &&
           mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token);
    finish_terminal_case(&c);
    puts("PASS terminal hole ENOMEM, invalid/protected output and real backend EIO EFAULT; slot intact");
}

/* A foreign slot operation may defer the copy, but is never cancelled or
 * mistaken for an allocation failure. */
static void storage_contention(void)
{
    struct terminal_case c=make_terminal_case(4);
    pressure_baseline_pages=live_pages+1;quota_enforced=true;gate_contended=true;
    struct trap_frame f=case_frame(&c);unsigned attempts=0,writes_before=gate_writes;
    invoke_one(&f,&attempts);
    assert(f.rax==LINUX_SYS_MINCORE && c.task->state==TASK_BLOCKED &&
           !c.task->waiting_queue && mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token &&
           !gate_reads && gate_writes==writes_before);
    gate_contended=false;
    assert(!invoke(&f,&attempts) && attempts==2 && gate_reads==1 && gate_writes==writes_before);
    finish_terminal_case(&c);
    puts("PASS storage contention retries the owned slot without reclaim");
}

/* A completed failed scan and a persistent allocator failure both terminate;
 * a rising free count by itself is never counted as a successful copy. */
static void unrecoverable(bool persistent)
{
    struct terminal_case c=make_terminal_case(persistent ? 8 : 0);
    pressure_baseline_pages=live_pages;quota_enforced=true;
    if(persistent)atomic_store(&fail_alloc,true);
    struct trap_frame f=case_frame(&c);unsigned attempts=0,writes_before=gate_writes;
    assert(invoke(&f,&attempts)==-RELIEFOS_EFAULT && attempts>1);
    assert(mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token && !gate_reads);
    if(!persistent)assert(oom_calls && gate_writes==writes_before);
    else assert(gate_writes>writes_before);
    finish_terminal_case(&c);
    printf("PASS %s terminates after %u entries with original slot intact\n",
           persistent ? "persistent allocator failure" : "no eligible victim",attempts);
}

/* A changed query VMA and a newly protected output are rechecked after the
 * scheduler gap, before reclaim or writing resumes. */
static void revalidate(bool protect_output)
{
    struct terminal_case c=make_terminal_case(4);
    pressure_baseline_pages=live_pages;quota_enforced=true;
    struct trap_frame f=case_frame(&c);unsigned attempts=0;
    invoke_one(&f,&attempts);
    assert(f.rax==LINUX_SYS_MINCORE && c.task->state==TASK_BLOCKED && c.task->waiting_queue);
    unsigned writes_before=gate_writes;
    if(protect_output)c.task->vmas[0].prot=LINUX_PROT_READ;
    else c.task->vmas[1].used=0;
    assert(invoke(&f,&attempts)==(protect_output ? -RELIEFOS_EFAULT : -RELIEFOS_ENOMEM));
    assert(attempts==2 && gate_writes==writes_before && !gate_reads &&
           mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token);
    c.task->vmas[1].used=1;finish_terminal_case(&c);
    printf("PASS %s revalidated after timed wait\n",protect_output ? "output protection" : "query mapping");
}

/* Replacing or interrupting a blocked syscall must detach its pressure wait
 * and discard its scalar attempt state before the lookup owner is released. */
static void cancel_wait(void)
{
    struct terminal_case c=make_terminal_case(4);
    pressure_baseline_pages=live_pages;quota_enforced=true;
    struct trap_frame f=case_frame(&c);unsigned attempts=0;
    invoke_one(&f,&attempts);
    assert(f.rax==LINUX_SYS_MINCORE && c.task->waiting_queue &&
           sched_pressure_cursor(c.task,true)->cache_attempt);
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    task_lookup_scope_cancel(c.task);
    assert(!c.task->waiting_queue && !c.task->lookup_scope.armed &&
           !sched_pressure_cursor(c.task,true)->cache_attempt &&
           !sched_pressure_cursor(c.task,false)->cache_attempt);
    kernel_execution_unlock_irqrestore(irq);
    assert(mm_swap_record_lookup(sched_task_as(c.task),c.page)==c.token);
    c.task->state=TASK_RUNNING;finish_terminal_case(&c);
    puts("PASS cancelled mincore detaches wait and resets both pressure cursors");
}

#ifndef PRESSURE_MINCORE_FIXTURE_ENTRY
#define PRESSURE_MINCORE_FIXTURE_ENTRY main
#endif
int PRESSURE_MINCORE_FIXTURE_ENTRY(int argc,char **argv)
{
    assert(argc==2);nx_enabled=true;page_cache_init();
    if(!strcmp(argv[1],"lazy"))lazy_output_shortage();
    else if(!strcmp(argv[1],"recovery"))swapped_output_shortage();
    else if(!strcmp(argv[1],"terminal"))terminal_controls();
    else if(!strcmp(argv[1],"contention"))storage_contention();
    else if(!strcmp(argv[1],"no-victim"))unrecoverable(false);
    else if(!strcmp(argv[1],"persistent"))unrecoverable(true);
    else if(!strcmp(argv[1],"query-revalidate"))revalidate(false);
    else if(!strcmp(argv[1],"output-revalidate"))revalidate(true);
    else if(!strcmp(argv[1],"cancel"))cancel_wait();
    else abort();
    return 0;
}

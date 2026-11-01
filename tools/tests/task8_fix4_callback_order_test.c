/* SPDX-License-Identifier: Apache-2.0 */
/* Real character mmap, VMA teardown and exec; CPU, TLB and MMIO are modeled. */
#define TASK8_FIX3_REUSED_BACKING 1
#define TASK8_FIX4_CALLBACK 1
#include "task8_fix2_device_mmap_test.c"

static struct task *callback_task;
static uint64_t callback_user, callback_pages;
static unsigned callback_flush_before,callback_cr3_loads;
static bool callback_destroyed;
static uint64_t callback_direct_pdpt[512] __attribute__((aligned(4096)));
static uint64_t callback_direct_pd[512] __attribute__((aligned(4096)));

/** @brief Model CR3 installation while recording the real exec teardown order.
 * @param cr3 Selected physical root, ignored by this host-only CPU model.
 */
void x86_64_load_cr3(uint64_t cr3) { (void)cr3;++callback_cr3_loads; }
/** @brief Return the sole modeled CPU index. @return CPU zero. */
uint32_t smp_current_cpu(void) { return 0; }
/** @brief Report the host fixture's one online CPU. @return One CPU. */
uint32_t smp_cpu_count(void) { return 1; }
/** @brief Query the host fixture's sole CPU. @param cpu Numeric index.
 * @return True only for CPU zero.
 */
bool smp_cpu_online(uint32_t cpu) { return cpu==0; }
/** @brief Model cancellation at exec before the original MM is retired.
 * @param task Original task whose new image is being installed.
 */
void graphics_service_cancel_task_continuation(struct task *task) { assert(task==current); }

/* Non-device exit dependencies are controlled host boundaries. The scheduler,
 * address-space destroy, VMA teardown and provider callbacks remain real. */
/** @brief Drain no host-modeled device requests. @param pid Original task ID. */
void storage_drain_task_io(uint32_t pid) { (void)pid;assert(!scheduler_lock.state); }
/** @brief Report the modeled outer execution transaction. @return True. */
bool kernel_execution_is_outermost(void) { return true; }
/** @brief Cancel no pending loader state. @param task Original task. */
void userland_exec_cancel(struct task *task) { assert(task); }
/** @brief Complete the modeled empty robust list. @param task Original task.
 * @return One, indicating cleanup completed.
 */
int futex_task_exit_step(struct task *task) { assert(task);return 1; }
/** @brief Complete the modeled no-swap empty robust list. @param task Original task. */
void futex_task_exit(struct task *task) { assert(task); }
/** @brief Cancel no host record lock. @param task Original task. */
void syscall_record_lock_cancel(struct task *task) { assert(task); }
/** @brief Reject any unexpected wait queue. @param queue Queue. @param task Task. */
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{ (void)queue;(void)task;abort(); }
/** @brief Cancel an empty socket receive. @param task Original task. @return Zero. */
uint64_t task_socket_cancel_receive(struct task *task) { assert(task);return 0; }
/** @brief Cancel an empty syscall-file slot. @param task Original task. */
void task_release_syscall_file(struct task *task) { assert(task); }
/** @brief Release no semaphore undo state. @param task Original task. */
void task_sysv_sem_exit(struct task *task) { assert(task); }
/** @brief Model session PTY cleanup. @param tgid Original task group. */
void pty_process_session_exit(uint32_t tgid) { (void)tgid; }
/** @brief Model descriptor-table cleanup. @param task Original task. */
void syscall_release_task_files(struct task *task) { assert(task && !scheduler_lock.state); }
/** @brief Model empty supplementary groups. @param task Original task. */
void task_groups_release(struct task *task) { assert(task); }
/** @brief Model no owned GPU objects. @param pid Original task ID. */
void svga_gpu_release_owner(uint32_t pid) { (void)pid; }
/** @brief Model PTY process cleanup. @param pid Original task ID. */
void pty_process_exit(uint32_t pid) { (void)pid; }
/** @brief Model no pending fatal signal. @param task Original task.
 * @return Zero because this fixture injects no signal.
 */
int kernel_signal_fatal_pending(const struct task *task) { assert(task);return 0; }
/** @brief Model flushing an empty signal set. @param task Original task.
 * @param process Process-wide selector. @param mask Signal mask.
 */
void kernel_signal_flush(struct task *task,bool process,uint64_t mask)
{ assert(task);(void)process;(void)mask; }

/** @brief Check every retired page remains UC throughout the final callback. */
static void fix4_assert_callback(void)
{
    assert(callback_task && !mmio_lock.state && !scheduler_lock.state);
    if(callback_destroyed)assert(!sched_task_as(callback_task)->pml4);
    else {
        assert(user_tlb_flush_calls>callback_flush_before);
        for(uint64_t i=0;i<callback_pages;++i)
            assert(!address_space_user_page_phys(sched_task_as(callback_task),callback_user+i*4096));
    }
    for(uint64_t i=0;i<callback_pages;++i) {
        struct paging_cache_claim probe={0};uint64_t flags=UINT64_MAX;
        assert(paging_cache_claim_select(&probe,device_base+i*4096,&flags));
        assert(flags==expected_cache);paging_cache_claim_release(&probe);
    }
}

enum callback_action { CALLBACK_UNMAP, CALLBACK_EXIT_TEARDOWN, CALLBACK_EXEC, CALLBACK_FORK };
/** @brief Exercise split/full retirement against high or direct-map UC pages.
 * @param action Real unmap, exit teardown, exec replacement or fork survivor.
 * @param low Use direct-map PCI pages when true, high pages otherwise.
 * @param split Create multiple independently owned metadata slices when true.
 */
static void exercise(enum callback_action action,bool low,bool split)
{
    uint64_t count=split ? 4 : 2,length=count*4096;
    device_base=low ? (6ULL<<30) : 0xc000000000ULL;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    if(low) {
        kernel_pml4[256]=(uintptr_t)callback_direct_pdpt|3;
        callback_direct_pdpt[6]=(uintptr_t)callback_direct_pd|3;
        callback_direct_pd[0]=device_base|PAGE_SIZE_FLAG|3;
    } else kernel_pml4[256]=0;
    mmio_dynamic_tables_ready=true;provider_releases=provider_calls=0;
    setup(1);clear_setup_vmas();current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    uint64_t user=base+0x1000000;
    expected_cache=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    void *pci=pci_map_mmio_lease(device_base,length);assert(pci);
    assert(syscall_mm_mmap(user,length,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0)==(int64_t)user);
    pci_unmap_mmio(pci,length);
    if(split) {
        assert(!syscall_mm_mprotect(user,4096,LINUX_PROT_READ));
        assert(!syscall_mm_mprotect(user+8192,4096,LINUX_PROT_READ));
        struct task_vma *a=task_vma_containing(current,user,user+4096);
        for(uint64_t i=1;i<count;++i) {
            struct task_vma *v=task_vma_containing(current,user+i*4096,user+(i+1)*4096);
            assert(v && v!=a && v->device_mapping==a->device_mapping);
        }
    }
    assert(policy_count()==count);
    struct task *parent=current,*child=NULL;
    if(action==CALLBACK_FORK) {
        child=calloc(1,sizeof(*child));assert(child);child->kind=TASK_KIND_USER;
        assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
        for(uint32_t i=0;i<sched_task_vma_capacity(parent);++i) {
            const struct task_vma *src=sched_task_vma_at(parent,i);
            if(!src || !src->used)continue;
            struct task_vma *dst=sched_task_vma_at(child,i);assert(dst);
            *dst=*src;dst->device_mapping=NULL;assert(!sched_task_vma_retain_device(dst,src));
        }
        assert(!syscall_mm_munmap(user,length));assert(!provider_releases);
        current=child;
    }
    callback_task=current;callback_user=user;callback_pages=count;
    callback_flush_before=user_tlb_flush_calls;
    callback_destroyed=action==CALLBACK_EXIT_TEARDOWN || action==CALLBACK_EXEC;
    if(action==CALLBACK_EXIT_TEARDOWN) {
        current->running_cpu=SCHED_CPU_NONE;current->state=TASK_EXITED;
        sched_release_task_resources(current);
        assert(current->flags&TASK_FLAG_RESOURCES_RELEASED);
    } else if(action==CALLBACK_EXEC) {
        struct address_space replacement={0};assert(address_space_create(&replacement));
        unsigned prior_loads=callback_cr3_loads;
        sched_exec_replace_mm(current,&replacement);
        assert(callback_cr3_loads==prior_loads+1);
    } else assert(!syscall_mm_munmap(user,length));
    assert(provider_releases==1 && !policy_count());callback_task=NULL;
    if(child) { address_space_destroy(sched_task_as(child));sched_task_vma_release(child);free(child);current=parent; }
    task_regular_io_stage_cancel(current);task_regular_io_stage_cancel(current);
    character_file_close(file.character_file,&file);file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    if(low) {
        uint64_t entry=callback_direct_pd[0];assert(!(entry&PAGE_SIZE_FLAG));
        mm_free_page(entry&RELIEFNT_PHYS_ADDR_MASK);callback_direct_pd[0]=0;
        kernel_pml4[256]=0;
    }
    teardown();assert(!cache_claims);
}

/** @brief Verify last callbacks without any extra raw provider reference.
 * @return Zero after all modeled high and direct-map ownership is retired.
 */
int main(void)
{
    for(unsigned low=0;low<2;++low) {
        exercise(CALLBACK_UNMAP,low,false);exercise(CALLBACK_UNMAP,low,true);
        exercise(CALLBACK_EXIT_TEARDOWN,low,true);exercise(CALLBACK_EXEC,low,true);
        exercise(CALLBACK_FORK,low,true);
    }
    puts("PASS unsplit, multisplit, teardown, exec and fork callback ordering");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Real clone scheduler and page-table controls for TID COW ownership.
 * CPU instructions, physical storage and user address checks use host models.
 */
#define TASK8_CLONE_TID_TEST 1
#include "sysv_shm_test.c"
#include <reliefnt/syscall.h>

#if defined(CONFIG_SWAP) && !defined(TASK12_FORK_RECLAIM_FIXTURE)
/** @brief Resident-only preparation for this fixture. */
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{
    assert(task && address && size && writing && kernel_execution_is_outermost());
    return 0;
}
/** @brief Trap unexpected swap leaves in this resident-only fixture. */
int swap_slot_get(uint64_t token) { (void)token;abort(); }
/** @brief Trap unexpected swap ownership in this resident-only fixture. */
void swap_slot_put(uint64_t token) { (void)token;abort(); }
#endif

/** @brief Replace a fake physical page's allocation with one half of a host pair. */
static void install_pair_page(uint64_t physical,void *host)
{
    for(unsigned i=0;i<1024;++i) if(pages[i].refs && pages[i].address==physical) {
        free(pages[i].host);pages[i].host=host;return;
    }
    abort();
}

/** @brief Read a child task's bytes through its actual page table. */
static uint32_t child_word(struct task *task,uint64_t address)
{
    uint8_t bytes[4];
    for(unsigned i=0;i<4;++i) {
        uint64_t physical=address_space_user_page_phys(sched_task_as(task),address+i);
        assert(physical);
        bytes[i]=*((uint8_t *)mm_phys_to_virt(physical)+((address+i)&4095));
    }
    uint32_t value;memcpy(&value,bytes,sizeof(value));return value;
}

/** @brief Exercise synchronous parent and child TID writes and rollback.
 * @param shared Clone shares the parent's address space.
 * @param vfork Clone also blocks the parent until child completion.
 * @param child_cross Child TID crosses two independently COW pages.
 */
static void run_case(bool shared,bool vfork,bool child_cross)
{
    struct sched_task_object parent_object={0},child_object={0};
    struct task *parent=&parent_object.task,*child=&child_object.task;
    task_zero(parent);task_zero(child);
    parent->pid=parent->tgid=801;parent->kind=TASK_KIND_USER;
    parent->state=TASK_RUNNING;parent->running_cpu=0;task_init_limits(parent);
    child->state=TASK_EXITED;child->running_cpu=SCHED_CPU_NONE;
    child->flags=TASK_FLAG_RESOURCES_RELEASED;
    struct task *entries[]={parent,child};tasks=entries;task_count=task_capacity=2;
    current_pid[0]=parent->pid;current_task[0]=parent;next_pid=1;
    nx_enabled=true;empty_clone_boundary=true;
    assert(address_space_create(sched_task_as(parent)));
    clone_tid_pair_bytes=4*4096;clone_tid_pair=aligned_alloc(4096,clone_tid_pair_bytes);assert(clone_tid_pair);
    memset(clone_tid_pair,0x44,clone_tid_pair_bytes);
    uint64_t p0=mm_alloc_page(),p1=mm_alloc_page(),p2=mm_alloc_page(),p3=mm_alloc_page();
    assert(p0 && p1 && p2 && p3);
    install_pair_page(p0,clone_tid_pair);install_pair_page(p1,(char *)clone_tid_pair+4096);
    install_pair_page(p2,(char *)clone_tid_pair+8192);install_pair_page(p3,(char *)clone_tid_pair+12288);
    uint64_t base=(uintptr_t)clone_tid_pair;
    assert(address_space_map_user_page(sched_task_as(parent),base,p0,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(sched_task_as(parent),base+4096,p1,RELIEFNT_PAGE_WRITABLE));
    uint64_t child_base=base+8192;
    assert(address_space_map_user_page(sched_task_as(parent),child_base,p2,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(sched_task_as(parent),child_base+4096,p3,RELIEFNT_PAGE_WRITABLE));
    uint64_t parent_tid=base+4094,child_tid=child_base+(child_cross ? 4094 : 64);
    uint64_t clone_flags=17|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID;
    if(shared)clone_flags|=CLONE_VM;
    if(vfork)clone_flags|=CLONE_VFORK;
    uint64_t irq;
    if(!shared) {
        for(int budget=0;budget<5;++budget) {
            shm_physical_budget=budget;kernel_execution_lock_irqsave(&irq);
            assert(syscall_clone_prepared(&parent->frame,clone_flags,0,parent_tid,child_tid,0)==-LINUX_ENOMEM);
            kernel_execution_unlock_irqrestore(irq);shm_physical_budget=-1;
            assert(!writable_aliases && child->state==TASK_EXITED);
            assert(address_space_user_page_phys(sched_task_as(parent),base)==p0);
            assert(address_space_user_page_phys(sched_task_as(parent),base+4096)==p1);
            uint32_t before;memcpy(&before,(void *)(uintptr_t)parent_tid,4);assert(before==0x44444444);
        }
    }
    kernel_execution_lock_irqsave(&irq);
    int64_t pid=syscall_clone_prepared(&parent->frame,clone_flags,0,parent_tid,child_tid,0);
    kernel_execution_unlock_irqrestore(irq);
    assert(pid>0 && child->pid==(uint32_t)pid && !writable_aliases);
    uint32_t written;memcpy(&written,(void *)(uintptr_t)parent_tid,4);assert(written==(uint32_t)pid);
    assert(child_word(child,child_tid)==(uint32_t)pid);
    if(shared) {
        assert(sched_task_as(parent)==sched_task_as(child));
        assert(child_word(child,parent_tid)==(uint32_t)pid);
        assert(child_word(parent,child_tid)==(uint32_t)pid);
    } else {
        assert(address_space_user_page_phys(sched_task_as(child),base)!=p0);
        assert(address_space_user_page_phys(sched_task_as(child),base+4096)!=p1);
        assert(address_space_user_page_phys(sched_task_as(child),child_base)!=p2);
        if(child_cross)assert(address_space_user_page_phys(sched_task_as(child),child_base+4096)!=p3);
        assert(child_word(child,parent_tid)==0x44444444);
        assert(child_word(parent,child_tid)==0x44444444);
        assert(address_space_user_page_writable(sched_task_as(parent),base));
        assert(address_space_user_page_writable(sched_task_as(parent),base+4096));
    }
    if(vfork) {
        assert(parent->state==TASK_BLOCKED && child->vfork_parent==parent);
        sched_vfork_child_done(child);assert(parent->state==TASK_READY && !child->vfork_parent);
    }
    if(shared) {
        assert(child->shared_mm==parent->shared_mm && parent->shared_mm->references==2);
        --child->shared_mm->references;child->shared_mm=NULL;
        address_space_destroy(&parent->shared_mm->as);kernel_free(parent->shared_mm);parent->shared_mm=NULL;
    } else {
        address_space_destroy(sched_task_as(child));sched_task_vma_release(child);
        address_space_destroy(sched_task_as(parent));
    }
    sched_task_fs_release(child);task_groups_release(child);
    syscall_release_task_files(child);
    kernel_free(child->exec_argv);kernel_free(child->exec_envp);kernel_free(child->exec_data);
    sched_task_identity_detach(child);
    free(clone_tid_pair);clone_tid_pair=NULL;clone_tid_pair_bytes=0;
    assert(!live_pages());
    current_task[0]=NULL;current_pid[0]=0;tasks=NULL;task_count=task_capacity=0;
}

#ifndef TASK8_FIX3_DEVICE_FORK
int main(void)
{
    run_case(false,false,false);run_case(false,false,true);
    run_case(true,false,true);run_case(true,true,true);
    puts("PASS actual clone scheduler and page tables: cross-page parent and child TIDs, COW, shared/vfork, OOM rollback");
    return 0;
}
#endif

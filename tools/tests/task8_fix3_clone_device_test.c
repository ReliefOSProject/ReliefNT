/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Actual scheduler fork keeps each DEVICE page policy live.
 * CPU, physical backing and TLB effects use the inherited host models.
 */
#define TASK8_FIX3_DEVICE_FORK 1
#include "task8_clone_tid_real_test.c"
#include <reliefnt/device_mapping.h>

struct fix3_fork_backing { uint64_t physical; unsigned released; };
/** @brief Release the model provider after the last typed owner.
 * @param context Original fixture backing.
 */
static void fix3_fork_release(void *context)
{ ++((struct fix3_fork_backing *)context)->released; }
/** @brief Resolve one offset of the model provider.
 * @param context Original fixture backing. @param offset Aligned byte offset.
 * @return Fixture physical page.
 */
static uint64_t fix3_fork_physical(void *context,uint64_t offset)
{ return ((struct fix3_fork_backing *)context)->physical+offset; }

/** @brief Fork an actual two-page DEVICE VMA and retire B independently.
 * @return Zero after parent, child, policy owners and provider are retired.
 */
int main(void)
{
    struct sched_task_object parent_object={0},child_object={0};
    struct task *parent=&parent_object.task,*child=&child_object.task;
    task_zero(parent);task_zero(child);task_init_limits(parent);
    parent->pid=parent->tgid=801;parent->kind=TASK_KIND_USER;
    parent->state=TASK_RUNNING;parent->running_cpu=0;
    child->state=TASK_EXITED;child->running_cpu=SCHED_CPU_NONE;
    child->flags=TASK_FLAG_RESOURCES_RELEASED;
    struct task *entries[]={parent,child};tasks=entries;task_count=task_capacity=2;
    current_pid[0]=parent->pid;current_task[0]=parent;next_pid=1;
    nx_enabled=true;empty_clone_boundary=true;
    assert(address_space_create(sched_task_as(parent)));
    struct fix3_fork_backing backing={.physical=0xc000000000ULL};
    const struct device_mapping_operations ops={.release=fix3_fork_release,
                                                 .physical=fix3_fork_physical};
    const struct device_mapping_info info={.length=8192,.max_prot=3,.can_split=true,.can_move=true};
    struct device_mapping *view=NULL;assert(!device_mapping_create(NULL,&ops,&backing,&info,&view));
    assert(!device_mapping_admit_view(&view));
    uint64_t user=RELIEFNT_USER_BASE+0x2000000,cache;
    for(unsigned i=0;i<2;++i) {
        assert(!device_mapping_cache_page(view,i*4096,backing.physical+i*4096,&cache) && !cache);
        assert(address_space_map_user_page(sched_task_as(parent),user+i*4096,
               backing.physical+i*4096,RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_SHARED|
               RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    }
    *sched_task_vma_at(parent,0)=(struct task_vma){.used=1,.start=user,.end=user+8192,
        .prot=3,.max_prot=3,.flags=TASK_VMA_FLAG_DEVICE|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_MMAP,
        .device_physical=backing.physical,.device_mapping=view};
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    fix3_fail_clone_after_vmas=true;
    assert(sched_clone_current(&parent->frame,17,0,0,0,0)==-LINUX_ENOMEM);
    fix3_fail_clone_after_vmas=false;
    assert(child->state==TASK_EXITED && !backing.released);
    assert(address_space_user_page_phys(sched_task_as(parent),user+4096)==backing.physical+4096);
    int64_t pid=sched_clone_current(&parent->frame,17,0,0,0,0);
    kernel_execution_unlock_irqrestore(irq);
    assert(pid>0 && child->pid==(uint32_t)pid);
    assert(sched_task_vma_at(child,0)->device_mapping==view);
    assert(address_space_user_page_phys(sched_task_as(child),user+4096)==backing.physical+4096);
    assert(!syscall_mm_munmap(user+4096,4096));
    struct paging_cache_claim uc={0};
    assert(!paging_cache_claim_exact(&uc,backing.physical+4096,4096,
                                     RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    current_pid[0]=child->pid;current_task[0]=child;
    assert(!syscall_mm_munmap(user+4096,4096));
    assert(paging_cache_claim_exact(&uc,backing.physical+4096,4096,
                                    RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    paging_cache_claim_release(&uc);
    assert(address_space_user_page_phys(sched_task_as(child),user)==backing.physical);
    current_pid[0]=parent->pid;current_task[0]=parent;
    address_space_destroy(sched_task_as(child));sched_task_vma_release(child);
    sched_task_fs_release(child);task_groups_release(child);syscall_release_task_files(child);
    kernel_free(child->exec_argv);kernel_free(child->exec_envp);kernel_free(child->exec_data);
    sched_task_identity_detach(child);
    assert(!backing.released);
    assert(!syscall_mm_munmap(user,4096));assert(backing.released==1);
    address_space_destroy(sched_task_as(parent));sched_task_vma_release(parent);
    assert(!live_pages() && !cache_claims);
    current_task[0]=NULL;current_pid[0]=0;tasks=NULL;task_count=task_capacity=0;
    puts("PASS actual scheduler fork retains and retires DEVICE page policies independently");
    return 0;
}

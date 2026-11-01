/* SPDX-License-Identifier: Apache-2.0 */
/* Real MAP_FIXED character admission with a retained other view and stage. */
#define TASK8_FIX3_REUSED_BACKING 1
#include "task8_fix2_device_mmap_test.c"

/** @brief Test a selected allocation failure or successful replacement.
 * @param argc Argument count. @param argv Failure ordinal, or -1 for success.
 * @return Zero after all original and prospective owners are retired.
 */
int main(int argc,char **argv)
{
    assert(argc==2);int failure=atoi(argv[1]);
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();
    current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    struct device_mapping_info info={.length=8192,.max_prot=3,.can_split=true,.can_move=true};
    expected_cache=0;
    assert(!device_mapping_create(NULL,&backing_ops,&device_base,&info,&reused_backing));
    uint64_t a=base+0x1000000,target=a+0x400000;
    fix2_device_user_start=a;fix2_device_user_end=a+4096;
    assert(syscall_mm_mmap(a,4096,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0)==(int64_t)a);
    assert(!regular_stage_begin(current,a,64,false));
    struct regular_io_stage *stage=regular_stage_find(current);assert(stage);
    struct task_vma *av=task_vma_containing(current,a,a+4096);assert(av);
    struct device_mapping *aview=av->device_mapping;
    uint64_t aphys=address_space_user_page_phys(sched_task_as(current),a);
    uint64_t aflags=address_space_user_page_flags(sched_task_as(current),a);
    uint64_t anon=LINUX_MAP_PRIVATE|LINUX_MAP_ANONYMOUS|LINUX_MAP_FIXED;
    assert(syscall_mm_mmap(target,8192,3,anon,UINT64_MAX,0)==(int64_t)target);
    assert(syscall_handle_task_page_fault(current,target,6));
    assert(syscall_handle_task_page_fault(current,target+4096,6));
    assert(!syscall_mm_mprotect(target+4096,4096,LINUX_PROT_READ));
    uint64_t frame[2],flags[2];
    for(unsigned i=0;i<2;++i) {
        frame[i]=address_space_user_page_phys(sched_task_as(current),target+4096*i);
        flags[i]=address_space_user_page_flags(sched_task_as(current),target+4096*i);
        *(unsigned char *)mm_phys_to_virt(frame[i])=(unsigned char)(0x51+i);
    }
    unsigned claims=policy_count();
    fail_after=failure;
    int64_t ret=syscall_mm_mmap(target,8192,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0);
    fail_after=-1;
    if(failure>=0) {
        assert(ret==-RELIEFOS_ENOMEM);
        for(unsigned i=0;i<2;++i) {
            assert(address_space_user_page_phys(sched_task_as(current),target+4096*i)==frame[i]);
            assert(address_space_user_page_flags(sched_task_as(current),target+4096*i)==flags[i]);
            assert(*(unsigned char *)mm_phys_to_virt(frame[i])==(unsigned char)(0x51+i));
            struct task_vma *v=task_vma_containing(current,target+4096*i,target+4096*(i+1));
            assert(v && (v->flags&TASK_VMA_FLAG_ANON) && v->prot==(i?LINUX_PROT_READ:3));
        }
        assert(policy_count()==claims);
    } else {
        assert(ret==(int64_t)target);
        assert(address_space_user_page_phys(sched_task_as(current),target)==device_base);
        assert(address_space_user_page_phys(sched_task_as(current),target+4096)==device_base+4096);
    }
    av=task_vma_containing(current,a,a+4096);
    assert(av && av->device_mapping==aview);
    assert(address_space_user_page_phys(sched_task_as(current),a)==aphys);
    assert(address_space_user_page_flags(sched_task_as(current),a)==aflags);
    assert(regular_stage_find(current)==stage && !provider_releases);
    assert(!syscall_mm_munmap(target,8192));
    task_regular_io_stage_cancel(current);task_regular_io_stage_cancel(current);
    assert(!syscall_mm_munmap(a,4096));
    struct device_mapping *last=reused_backing;reused_backing=NULL;
    device_mapping_put(last);assert(provider_releases==1 && !policy_count());
    character_file_close(file.character_file,&file);file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    teardown();assert(!cache_claims);
    return 0;
}

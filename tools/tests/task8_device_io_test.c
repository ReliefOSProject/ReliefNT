/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Actual regular provider and device-mapping lease through retry.
 * Host pages model physical/direct aliases; no real device/MMIO is exercised.
 */
#define main inherited_task8_device_io_main
#include "regular_file_io_test.c"
#undef main
#include <reliefnt/device_mapping.h>
#include <reliefnt/swap.h>
static unsigned releases;
static bool direct_coverage=true;
static bool alias_allocation_fails;
static uint64_t direct_cache,alias_cache;
static unsigned temporary_aliases;
static void device_release(void *context) { assert(context==fixture_alias);++releases; }
static uint64_t device_physical(void *context,uint64_t offset)
{ assert(context==fixture_alias && !offset);return (uintptr_t)fixture_alias; }
static const struct device_mapping_operations ops={.release=device_release,.physical=device_physical};
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task;return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task,uint32_t index)
{ assert(index<SCHED_TASK_VMA_MAX);return &sched_task_mm(task)->vmas[index]; }
bool paging_kernel_direct_cache_flags(uint64_t physical,uint64_t *flags)
{ assert(physical==(uintptr_t)fixture_alias);*flags=direct_cache;return direct_coverage; }
void *paging_kernel_direct_map(uint64_t physical)
{ assert(physical==(uintptr_t)fixture_alias);return fixture_alias; }
void *paging_ioremap_device_page(uint64_t physical,uint64_t flags)
{ assert(physical==(uintptr_t)fixture_alias && !direct_coverage);alias_cache=flags;
  if(alias_allocation_fails)return NULL;++temporary_aliases;return fixture_alias; }
bool paging_iounmap(void *pointer,uint64_t bytes)
{ assert(pointer==fixture_alias && bytes==4096 && temporary_aliases);--temporary_aliases;return true; }
#ifdef CONFIG_SWAP
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool write)
{ (void)task;(void)address;(void)size;(void)write;return 0; }
#endif

int main(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=10;task->kind=TASK_KIND_USER;
    int shared=memfd_create("task8-device-owner",0);assert(shared>=0 && !ftruncate(shared,4096));
    fixture_user=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,shared,0);
    fixture_alias=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,shared,0);
    assert(fixture_user!=MAP_FAILED && fixture_alias!=MAP_FAILED);fixture_refs=1;fixture_device=true;
    struct device_mapping_info info={.length=4096,.max_prot=3,.can_split=true,.can_move=true};
    struct device_mapping *mapping=NULL;
    assert(!device_mapping_create(NULL,&ops,fixture_alias,&info,&mapping));
    sched_task_mm(task)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)fixture_user,
        .end=(uintptr_t)fixture_user+4096,.flags=TASK_VMA_FLAG_DEVICE,
        .prot=3,.max_prot=3,.device_mapping=mapping,.device_physical=(uintptr_t)fixture_alias};
    struct task_file file={.used=1,.flags=RELIEFOS_O_RDWR,.node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=2}};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&provider_ops,disk,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
    expected_cursor=&file.read_cursor;task->syscall_file=&file;task->syscall_file_number=LINUX_SYS_READ;
    memset(disk,0x71,64);error_offset=0;injected_error=-LINUX_EAGAIN;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==-LINUX_EAGAIN);
    assert(fixture_refs==1 && !releases);
    /* The original device is held even if the peer removes its VMA. */
    assert(mmap(fixture_user,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==fixture_user);
    sched_task_mm(task)->vmas[0]=(struct task_vma){0};device_mapping_put(mapping);
    assert(!releases);
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==64);
    for(unsigned i=0;i<64;++i)assert(!fixture_user[i] && fixture_alias[i]==0x71);
    assert(releases==1 && fixture_refs==1);

    /* Read-only mappings remain usable as input, and their accepted bytes are
     * a snapshot even if the VMA/provider is gone while the write waits. */
    assert(mmap(fixture_user,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,shared,0)==fixture_user);
    assert(!device_mapping_create(NULL,&ops,fixture_alias,&info,&mapping));
    sched_task_mm(task)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)fixture_user,
        .end=(uintptr_t)fixture_user+4096,.flags=TASK_VMA_FLAG_DEVICE,
        .prot=1,.max_prot=3,.device_mapping=mapping};
    fixture_readonly=true;unsigned old_reads=provider_reads;file.offset=0;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==-LINUX_EFAULT);
    assert(provider_reads==old_reads);
    memset(fixture_alias,0x93,64);file.offset=128;task->syscall_file_number=LINUX_SYS_WRITE;
    error_offset=128;injected_error=-LINUX_EAGAIN;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,true,false)==-LINUX_EAGAIN);
    sched_task_mm(task)->vmas[0]=(struct task_vma){0};device_mapping_put(mapping);assert(releases==2);
    assert(mmap(fixture_user,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==fixture_user);
    memset(fixture_user,0x32,64);
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,true,false)==64);
    for(unsigned i=0;i<64;++i)assert(disk[128+i]==0x93 && fixture_user[i]==0x32);
    fixture_readonly=false;

    /* A high page gets a bounded temporary alias with the user leaf policy;
     * cancellation drains the provider and drops the final backing once. */
    assert(mmap(fixture_user,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,shared,0)==fixture_user);
    assert(!device_mapping_create(NULL,&ops,fixture_alias,&info,&mapping));
    sched_task_mm(task)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)fixture_user,
        .end=(uintptr_t)fixture_user+4096,.flags=TASK_VMA_FLAG_DEVICE,.prot=3,
        .max_prot=3,.device_mapping=mapping};
    direct_coverage=false;fixture_device_cache_flags=RELIEFNT_PAGE_PAT|RELIEFNT_PAGE_PWT;
    file.offset=0;task->syscall_file_number=LINUX_SYS_READ;error_offset=0;injected_error=-LINUX_EAGAIN;
    alias_allocation_fails=true;old_reads=provider_reads;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==-LINUX_ENOMEM);
    assert(provider_reads==old_reads && !temporary_aliases && releases==2);
    alias_allocation_fails=false;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==-LINUX_EAGAIN);
    assert(temporary_aliases==1 && alias_cache==fixture_device_cache_flags && fixture_refs==1);
    sched_task_mm(task)->vmas[0]=(struct task_vma){0};device_mapping_put(mapping);assert(releases==2);
    storage_drain_task_io(task->pid);task_regular_io_stage_cancel(task);
    assert(!temporary_aliases && releases==3);task->regular_io.active=false;file.io_owner=0;

    /* FB0 backing is permanent, with the actual direct-map cache policy. */
    direct_coverage=true;direct_cache=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    fixture_device_cache_flags=direct_cache;
    sched_task_mm(task)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)fixture_user,
        .end=(uintptr_t)fixture_user+4096,.flags=TASK_VMA_FLAG_DEVICE,.prot=3,.max_prot=3,
        .file_node={.first_cluster=STORAGE_DEV_KIND_FB0}};
    file.offset=0;task->syscall_file_number=LINUX_SYS_READ;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==64);
    assert(fixture_refs==1 && releases==3);
    /* Existing conflicting aliases are detected rather than adding a second
     * physical cache type; ordinary mmap now installs the direct policy. */
    fixture_device_cache_flags=0;file.offset=0;
    assert(syscall_regular_io(task,&file,(uintptr_t)fixture_user,64,0,false,false)==-LINUX_EIO);
    assert(fixture_refs==1 && !temporary_aliases);
    vfs_path_put(&file.path_ref);assert(!provider_pins);
    munmap(fixture_user,4096);munmap(fixture_alias,4096);close(shared);free(task);
    puts("PASS retained device mapping for pending regular read");return 0;
}

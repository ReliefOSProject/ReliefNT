/* SPDX-License-Identifier: Apache-2.0 */
/* Real pathname import, VFS walk, open entry and PTE/swap reclaim with a modeled
 * finite heap/physical quota, storage transport and VFS backend callback. */
#define PRESSURE_OPEN_FIXTURE 1
#define PRESSURE_MINCORE_FIXTURE_ENTRY pressure_mincore_unused_main
#include "memory_pressure_mincore_fix4_test.c"
#include "../../block/core/storage_internal.h"
#include <reliefnt/swap_internal.h>
#include <reliefnt/page_cache.h>

static struct vfs_namespace fixture_ns;
static struct vfs_path expected_root,expected_cwd;
static unsigned import_denials,walk_denials,workspace_denials,lookup_calls,proc_calls,search_calls;
static unsigned native_parent_calls,native_dir_calls,native_child_calls;
static uint32_t expected_fsuid=1234,proc_inode;
static bool deny_import,deny_walk,deny_workspace,provider_failure,track_lookup,native_mode,native_device_error;
static int provider_cookie;
static struct storage_volume fixture_volume;

int storage_select_node_volume(const struct storage_node *node,struct storage_volume **previous)
{ assert(node && previous && writer_owned());*previous=NULL;return 0; }
void storage_restore_volume(struct storage_volume *previous) { assert(!previous && writer_owned()); }
int storage_ext4_check_node(struct storage_volume *volume,const struct storage_node *node,struct ext4_inode_view *out)
{
    assert(volume==&fixture_volume && node && out && writer_owned());++native_parent_calls;
    *out=(struct ext4_inode_view){.mode=0040000};return 0;
}
int storage_ext4_dir_lookup_prepared(struct storage_volume *volume,uint64_t ino,const char *name,
                                     uint64_t *found,uint8_t *type,uint8_t *buffer)
{
    assert(volume==&fixture_volume && ino && !strcmp(name,"vmstat") && found && !type && buffer && writer_owned());
    ++native_dir_calls;if(native_device_error)return -ENOMEM;*found=500;return 0;
}
int storage_ext4_dir_lookup(struct storage_volume *volume,uint64_t ino,const char *name,uint64_t *found,uint8_t *type)
{ (void)volume;(void)ino;(void)name;(void)found;(void)type;abort(); }
int storage_ext4_read_inode(struct storage_volume *volume,uint64_t ino,struct ext4_inode_view *out)
{
    assert(volume==&fixture_volume && ino==500 && out && writer_owned());++native_child_calls;
    *out=(struct ext4_inode_view){.mode=0040000};return 0;
}
void storage_ext4_node_from_inode(struct storage_volume *volume,uint64_t ino,const struct ext4_inode_view *in,
                                  struct storage_node *out)
{
    assert(volume==&fixture_volume && ino==500 && in && out && writer_owned());
    *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=ino,.volume_id=volume->volume_id};
}
#include "ext_lookup.inc"
static const struct storage_filesystem_ops fixture_fs_ops={.lookup=ext_native_lookup};

static bool pressure_fixture_heap_admitted(size_t size)
{
    /* Model the two ordinary contiguous pages needed for PATH_MAX plus header. */
    if(deny_import && size>VFS_PATH_MAX && size<VFS_PATH_MAX+128 &&
       quota_enforced && live_pages+2>pressure_baseline_pages) {
        ++import_denials;return false;
    }
    /* The walker's PATH_MAX allocation needs two contiguous pages including
     * the production heap header; deny only this exact requested size. */
    if(deny_walk && size==VFS_PATH_MAX && quota_enforced &&
       live_pages+2>pressure_baseline_pages) {
        ++walk_denials;return false;
    }
    if(deny_workspace && size==3*VFS_PATH_MAX && quota_enforced &&
       live_pages+4>pressure_baseline_pages) {
        ++workspace_denials;return false;
    }
    return true;
}
static int fixture_lookup(void *context,const struct storage_node *parent,const char *name,
                          struct storage_node *out)
{
    static uint32_t next_inode=2;
    assert(context==&provider_cookie && parent && name && out);
    if(track_lookup) {
        struct task_lookup_path *slot=&current->lookup_scope.paths[0];
        assert(vfs_path_equal(&slot->root,&expected_root) &&
               vfs_path_equal(&slot->start,&expected_cwd));
        ++lookup_calls;
        if(!strcmp(name,"proc")) {
            ++proc_calls;assert(parent->first_cluster==vfs_path_node(&expected_cwd)->first_cluster);
        }
        else assert(!strcmp(name,"vmstat") && parent->first_cluster==proc_inode);
        if(provider_failure)return -ENOMEM;
        if(native_mode && !strcmp(name,"vmstat"))
            return storage_backend_lookup_vfs(&fixture_volume,parent,name,out);
    }
    *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=next_inode++};
    if(track_lookup && !strcmp(name,"proc"))proc_inode=out->first_cluster;
    return 0;
}
static int fixture_retain(void *context,const struct storage_node *node,void **pin)
{ assert(context==&provider_cookie && node && pin);*pin=NULL;return 0; }
static void fixture_release(void *context,const struct storage_node *node,void *pin)
{ assert(context==&provider_cookie && node && !pin); }
static bool fixture_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ assert(context==&provider_cookie);return a->first_cluster==b->first_cluster; }
static const struct vfs_backend_ops ops={
    .lookup=fixture_lookup,.retain=fixture_retain,.release=fixture_release,.same_node=fixture_same
};

int fs_permissions_check_path(const struct task *task,const struct vfs_path *path,uint32_t access,bool real)
{
    assert(task==current && task->fsuid==expected_fsuid && vfs_path_node(path));
    if(track_lookup)++search_calls;
    (void)access;(void)real;return 0;
}
struct task_file *task_file_for_io(struct task *task,int fd)
{ (void)task;(void)fd;abort(); }
struct task_pty_fd *task_pty_for_io(struct task *task,int fd)
{ (void)task;(void)fd;abort(); }
static int64_t pressure_fixture_open(struct task *task,uint64_t nr,uint64_t a,uint64_t b)
{
    int32_t dirfd=nr==LINUX_SYS_OPEN ? LINUX_AT_FDCWD : (int32_t)a;
    uint64_t pathname=nr==LINUX_SYS_OPEN ? a : b;
    struct vfs_lookup_result result={0};
    int rc=resolve_user_object_at_flags(&fixture_ns,task,dirfd,pathname,false,false,
                                        VFS_LOOKUP_FOLLOW,&result);
    if(rc)return rc;
    vfs_path_put(&result.path);return 42;
}

static void invoke_open_one(struct trap_frame *frame,unsigned *attempts)
{
    ++pressure_fixture_tick;current->state=TASK_RUNNING;current->running_cpu=0;
    if(current->waiting_queue)kernel_wait_queue_remove(current->waiting_queue,current);
    frame->rip=0x400002;frame->rax=LINUX_SYS_OPEN;
    (void)syscall_dispatch_frame(frame);
    assert(!writer_owned() && !reader_owned() && !user_page_fault_lock.state);
    assert(++*attempts<1800);
}
static int64_t invoke_open(struct trap_frame *frame,unsigned *attempts)
{
    do { invoke_open_one(frame,attempts); } while(frame->rax==LINUX_SYS_OPEN);
    return (int64_t)frame->rax;
}

int main(void)
{
    nx_enabled=true;page_cache_init();
    struct swap_area *area=setup_area();
    struct task *task=new_task();task->fsuid=1234;
    const uint64_t page=(uintptr_t)mincore_result,pathname=page+128,victims=0x30000000;
    memcpy((void *)(uintptr_t)pathname,"proc/vmstat",12);
    task->vmas[0]=(struct task_vma){.used=1,.start=page,.end=page+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    create_victims(task,victims,8);
    uint64_t phys=mm_alloc_page();assert(phys);
    assert(address_space_map_user_page(sched_task_as(task),page,phys,
                                      RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    prepare_task(task);

    struct storage_node root_node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};
    struct vfs_mount *mount=NULL;
    struct vfs_path old_root={0},old_cwd={0},new_root={0},new_cwd={0};
    assert(!vfs_mount_new(&ops,&provider_cookie,NULL,&root_node,&mount));
    assert(!vfs_namespace_init(&fixture_ns,mount));
    assert(!vfs_namespace_root(&fixture_ns,&old_root));
    assert(!vfs_path_lookup_child(&old_root,"old-cwd",&old_cwd));
    assert(!vfs_path_lookup_child(&old_root,"new-root",&new_root));
    assert(!vfs_path_lookup_child(&old_root,"new-cwd",&new_cwd));
    vfs_mount_put(mount); /* The namespace now owns its retained mount. */
    struct task_fs_paths *paths=sched_task_paths(task);
    vfs_path_get(&old_root,&paths->root);vfs_path_get(&old_cwd,&paths->cwd);
    expected_root=old_root;expected_cwd=old_cwd;track_lookup=true;
    fixture_volume=(struct storage_volume){.fs_ops=&fixture_fs_ops,.filesystem=STORAGE_FILESYSTEM_EXT4,
        .ext4={.block_size=4096}};

    struct trap_frame frame={.rax=LINUX_SYS_OPEN,.rdi=pathname,.rsi=LINUX_O_RDONLY,
        .rip=0x400002,.rsp=RELIEFNT_USER_TOP-8192,.cs=0x23,.ss=0x1b,.rflags=1ULL<<9};
    pressure_baseline_pages=live_pages;quota_enforced=true;deny_import=true;
    pressure_regular_contend_writes=true;unsigned attempts=0;
    invoke_open_one(&frame,&attempts);
    assert(frame.rax==LINUX_SYS_OPEN && task->waiting_queue && import_denials && !lookup_calls);
    assert(vfs_path_equal(&task->lookup_scope.paths[0].root,&old_root) &&
           vfs_path_equal(&task->lookup_scope.paths[0].start,&old_cwd));
    vfs_path_put(&paths->root);vfs_path_put(&paths->cwd);
    vfs_path_get(&new_root,&paths->root);vfs_path_get(&new_cwd,&paths->cwd);
    pressure_regular_contend_writes=false;
    assert(invoke_open(&frame,&attempts)==42 && lookup_calls==2 && gate_writes>=2);
    assert(!task->lookup_scope.armed && !task->waiting_queue);

    /* Fail the real walker's pending allocation before any search/backend.
     * Retain the captured name and identities across a pressure wait, then
     * change cwd, root, user bytes and credentials before the retry. */
    expected_root=new_root;expected_cwd=new_cwd;
    pressure_baseline_pages=live_pages;quota_enforced=true;deny_import=false;deny_walk=true;
    pressure_regular_contend_writes=true;attempts=0;
    unsigned before=lookup_calls,searches_before=search_calls,writes_before=gate_writes;
    invoke_open_one(&frame,&attempts);
    struct task_lookup_path *slot=&task->lookup_scope.paths[0];
    assert(frame.rax==LINUX_SYS_OPEN && task->waiting_queue && walk_denials && lookup_calls==before &&
           search_calls==searches_before && slot->name && !strcmp(slot->name,"proc/vmstat") &&
           (slot->flags&TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG) && !slot->walk &&
           vfs_path_equal(&slot->root,&new_root) && vfs_path_equal(&slot->start,&new_cwd));
    vfs_path_put(&paths->root);vfs_path_put(&paths->cwd);
    vfs_path_get(&old_root,&paths->root);vfs_path_get(&old_cwd,&paths->cwd);
    memcpy((void *)(uintptr_t)pathname,"changed/path",13);
    task->fsuid=expected_fsuid=4321;
    pressure_regular_contend_writes=false;
    assert(invoke_open(&frame,&attempts)==42 && lookup_calls==before+2 &&
           search_calls==searches_before+2 && gate_writes>=writes_before+2);
    assert(!task->lookup_scope.armed && !task->waiting_queue);
    memcpy((void *)(uintptr_t)pathname,"proc/vmstat",12);

    /* A provider ENOMEM occurs after lookup starts and must never be replayed. */
    quota_enforced=false;deny_import=false;deny_walk=false;
    expected_root=old_root;expected_cwd=old_cwd;provider_failure=true;
    before=lookup_calls;writes_before=gate_writes;attempts=0;
    assert(invoke_open(&frame,&attempts)==-ENOMEM && attempts==1);
    assert(lookup_calls==before+1 && gate_writes==writes_before && !task->lookup_scope.armed);
    provider_failure=false;

    /* A bad pathname remains EFAULT and never reaches the provider. */
    frame.rdi=1;before=lookup_calls;attempts=0;
    assert(invoke_open(&frame,&attempts)==-EFAULT && attempts==1 && lookup_calls==before);
    frame.rdi=pathname;

    /* The real native preallocation runs at the second walker component.
     * No parent inode or directory operation may run on allocation failure. */
    restore_victims(task,victims,8);
    native_mode=true;deny_workspace=true;pressure_baseline_pages=live_pages;quota_enforced=true;
    pressure_regular_contend_writes=true;attempts=0;
    before=lookup_calls;unsigned proc_before=proc_calls,parent_before=native_parent_calls;
    unsigned dir_before=native_dir_calls,child_before=native_child_calls;
    writes_before=gate_writes;searches_before=search_calls;
    invoke_open_one(&frame,&attempts);
    slot=&task->lookup_scope.paths[0];
    assert(frame.rax==LINUX_SYS_OPEN && task->waiting_queue && workspace_denials &&
           lookup_calls==before+2 && proc_calls==proc_before+1 && native_parent_calls==parent_before &&
           native_dir_calls==dir_before && native_child_calls==child_before && slot->walk &&
           (slot->flags&TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG) &&
           vfs_path_equal(&slot->root,&old_root) && vfs_path_equal(&slot->start,&old_cwd));
    vfs_path_put(&paths->root);vfs_path_put(&paths->cwd);
    vfs_path_get(&new_root,&paths->root);vfs_path_get(&new_cwd,&paths->cwd);
    memcpy((void *)(uintptr_t)pathname,"changed/path",13);
    task->fsuid=expected_fsuid=5678;
    pressure_regular_contend_writes=false;
    assert(invoke_open(&frame,&attempts)==42 && lookup_calls>=before+3 && proc_calls==proc_before+1 &&
           native_parent_calls==parent_before+1 && native_dir_calls==dir_before+1 &&
           native_child_calls==child_before+1 && search_calls>=searches_before+3 &&
           gate_writes>=writes_before+4 && !task->lookup_scope.armed && !task->waiting_queue);
    memcpy((void *)(uintptr_t)pathname,"proc/vmstat",12);
    expected_root=new_root;expected_cwd=new_cwd;quota_enforced=false;deny_workspace=false;

    /* An ENOMEM produced after the native directory operation begins remains
     * terminal: no reclaim and no provider replay. */
    native_device_error=true;before=lookup_calls;proc_before=proc_calls;
    parent_before=native_parent_calls;dir_before=native_dir_calls;child_before=native_child_calls;
    writes_before=gate_writes;attempts=0;
    assert(invoke_open(&frame,&attempts)==-ENOMEM && attempts==1 && lookup_calls==before+2 &&
           proc_calls==proc_before+1 && native_parent_calls==parent_before+1 &&
           native_dir_calls==dir_before+1 && native_child_calls==child_before && gate_writes==writes_before);
    native_device_error=false;

    /* A competing storage owner prevents this reclaim without being released. */
    pressure_baseline_pages=live_pages;quota_enforced=true;deny_workspace=true;
    pressure_fixture_storage_busy=true;parent_before=native_parent_calls;
    writes_before=gate_writes;attempts=0;
    assert(invoke_open(&frame,&attempts)==-ENOMEM && attempts==1 && pressure_fixture_storage_busy &&
           native_parent_calls==parent_before && gate_writes==writes_before && !task->lookup_scope.armed);
    pressure_fixture_storage_busy=false;

    /* Signal's release entry cancels this exact pending workspace wait and
     * retires the retained earlier directory component. */
    pressure_baseline_pages=live_pages;quota_enforced=true;deny_workspace=true;
    pressure_regular_contend_writes=true;attempts=0;
    invoke_open_one(&frame,&attempts);
    assert(frame.rax==LINUX_SYS_OPEN && task->waiting_queue && task->lookup_scope.armed &&
           (task->lookup_scope.paths[0].flags&TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG) &&
           task->lookup_scope.paths[0].name && task->lookup_scope.paths[0].walk);
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    task->pending_signals=1;task_release_syscall_file(task);task->pending_signals=0;
    assert(!task->waiting_queue && !task->lookup_scope.armed &&
           !sched_pressure_cursor(task,true)->cache_attempt);
    kernel_execution_unlock_irqrestore(irq);
    task->state=TASK_RUNNING;quota_enforced=false;deny_import=false;deny_walk=false;deny_workspace=false;
    pressure_regular_contend_writes=false;

    restore_victims(task,victims,8);
    vfs_path_put(&paths->root);vfs_path_put(&paths->cwd);
    vfs_path_put(&old_root);vfs_path_put(&old_cwd);
    vfs_path_put(&new_root);vfs_path_put(&new_cwd);
    assert(!vfs_namespace_destroy(&fixture_ns));
    destroy_task(task);assert(!live_pages && !swap_area_unpublish(area));swap_area_destroy(area);
    puts("PASS open path/walk/native workspace: bounded reclaim, held progress/identity, permission recheck, no provider replay, busy storage and signal cleanup");
    return 0;
}

#include "block_description_fixture.h"
/* SPDX-License-Identifier: Apache-2.0 */
/* Actual VFS/task resolver/DAC/tmpfs components; no pathname-success stubs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/syscall/internal.h"
#include <reliefnt/tmpfs.h>

struct timestamp_fixture {struct tmpfs_super *fs;unsigned lookups,updates;bool wait_update;};
static struct timestamp_fixture fixture;
static char empty_path[1];
static uintptr_t timestamp_input,timestamp_name;
static size_t timestamp_bytes,timestamp_name_bytes;
void *kernel_malloc(size_t bytes){return malloc(bytes);}
void kernel_free(void *memory){free(memory);}
uint64_t mm_total_memory_kib(void){return 65536;}
uint64_t mm_alloc_page(void){abort();}
void mm_free_page(uint64_t page){(void)page;abort();}
void mm_retain_page(uint64_t page){(void)page;abort();}
void *mm_phys_to_virt(uint64_t page){(void)page;abort();}
void sched_truncate_file_mappings(const struct storage_node *node,uint64_t size)
{(void)node;(void)size;abort();}
void page_cache_invalidate_node(const struct storage_node *node){(void)node;}
int time_wall_clock(struct reliefos_time_info *out)
{*out=(struct reliefos_time_info){.unix_seconds=1700000000};return 0;}
bool user_range_ok(uint64_t address,uint64_t bytes)
{
    if(address==(uintptr_t)empty_path && bytes<=sizeof(empty_path))return true;
    return (address>=timestamp_input && bytes<=timestamp_bytes && address-timestamp_input<=timestamp_bytes-bytes) ||
        (address>=timestamp_name && bytes<=timestamp_name_bytes && address-timestamp_name<=timestamp_name_bytes-bytes);
}
size_t user_strlen(const char *text,size_t maximum)
{for(size_t i=0;i<maximum;++i){if(!user_range_ok((uintptr_t)text+i,1))return maximum;if(!text[i])return i;}return maximum;}
/* RAM-only provider waits below have no physical command or registered queue. */
void storage_drain_task_io(uint32_t pid){assert(pid==51);}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{(void)queue;(void)task;abort();}
/* Timestamp-only lookup scopes never own executable deny-write guards. */
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;abort();}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;abort();}

static int timestamp_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    struct timestamp_fixture *f=context;++f->lookups;
    return tmpfs_lookup_child(f->fs,parent->first_cluster,name,out);
}
static int timestamp_retain(void *context,const struct storage_node *node,void **pin)
{
    struct timestamp_fixture *f=context;*pin=NULL;
    int rc=tmpfs_hold(f->fs,node->first_cluster);if(!rc)*pin=f;return rc;
}
static void timestamp_release(void *context,const struct storage_node *node,void *pin)
{struct timestamp_fixture *f=context;assert(pin==f);tmpfs_drop(f->fs,node->first_cluster);}
static bool timestamp_same(void *context,const struct storage_node *left,const struct storage_node *right)
{(void)context;return left->first_cluster==right->first_cluster;}
static int timestamp_permissions(void *context,const struct vfs_path *path,struct reliefos_permissions *value,bool write)
{return tmpfs_permissions(((struct timestamp_fixture *)context)->fs,vfs_path_node(path)->first_cluster,value,write);}
static int timestamp_update(void *context,const struct storage_node *node,int64_t atime,int64_t mtime,bool a,bool m)
{
    struct timestamp_fixture *f=context;
    if(f->wait_update){f->wait_update=false;return -RELIEFOS_EAGAIN;}
    int rc=tmpfs_utimens(f->fs,node->first_cluster,atime,mtime,a,m);if(!rc)++f->updates;return rc;
}
static int timestamp_root(void *context,struct vfs_mount **out)
{
    static const struct vfs_backend_ops ops={.lookup=timestamp_lookup,.retain=timestamp_retain,
        .release=timestamp_release,.same_node=timestamp_same,.permissions=timestamp_permissions,.utimens=timestamp_update};
    struct timestamp_fixture *f=context;struct storage_node root;
    int rc=tmpfs_lookup(f->fs,"/",&root);return rc ? rc : vfs_mount_new(&ops,context,NULL,&root,out);
}
int main(void)
{
    assert(!tmpfs_new(7,0,"size=0,nr_inodes=0",1000,1000,&fixture.fs));
    struct storage_node inode;
    assert(!tmpfs_create(fixture.fs,"/original",LINUX_S_IFREG|0600,NULL,&inode));
    struct reliefos_permissions owner={0600,1000,1000};
    assert(!tmpfs_permissions(fixture.fs,inode.first_cluster,&owner,true));
    assert(!vfs_system_bootstrap(timestamp_root,&fixture,NULL,0));
    struct vfs_namespace *ns=vfs_system_namespace();assert(ns);
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=51;task->euid=task->fsuid=1000;task->egid=task->fsgid=1000;
    assert(!vfs_namespace_root(ns,&task->fs_paths.root));
    vfs_path_get(&task->fs_paths.root,&task->fs_paths.cwd);
    struct vfs_path path={0};
    assert(!vfs_path_lookup(ns,&task->fs_paths.root,&task->fs_paths.cwd,"original",0,&path));
    struct task_file *file=&task->files[0];
    *file=(struct task_file){.used=1,.node=inode};
    task_file_bind_path(file,&path);vfs_path_put(&path);
    strcpy(file->path,"/deliberately/stale");
    assert(!tmpfs_unlink(fixture.fs,"/original",false));
    unsigned lookups=fixture.lookups;
    struct linux_timespec times[2]={{123,0},{456,0}};
    assert(!syscall_update_path_times(task,3,0,times,0,true));
    struct linux_stat_abi stat;
    assert(!tmpfs_stat(fixture.fs,inode.first_cluster,&stat) && stat.atime_sec==123 && stat.mtime_sec==456 && !stat.st_nlink);
    assert(fixture.updates==1 && fixture.lookups==lookups);
    assert(syscall_update_path_times(task,3,0,times,LINUX_AT_SYMLINK_NOFOLLOW,true)==-RELIEFOS_EINVAL);
    assert(syscall_update_path_times(task,4,0,times,0,true)==-RELIEFOS_EBADF);
    file->flags=TASK_FILE_FLAG_PATH;
    assert(syscall_update_path_times(task,3,0,times,0,true)==-RELIEFOS_EBADF);
    assert(!syscall_update_path_times(task,3,(uintptr_t)empty_path,times,LINUX_AT_EMPTY_PATH,true));
    file->flags=0;
    task->euid=0;task->fsuid=2000;
    assert(syscall_update_path_times(task,3,0,times,0,true)==-RELIEFOS_EPERM);
    task->euid=1000;
    assert(syscall_update_path_times(task,3,0,times,0,true)==-RELIEFOS_EPERM);
    task->cap_effective=1ULL<<CAP_FOWNER;
    assert(!syscall_update_path_times(task,3,0,times,0,true));
    task->cap_effective=0;times[0].tv_nsec=times[1].tv_nsec=1073741823;
    assert(syscall_update_path_times(task,3,0,times,0,false)==-RELIEFOS_EACCES);
    owner.mode=0666;assert(!tmpfs_permissions(fixture.fs,inode.first_cluster,&owner,true));
    assert(!syscall_update_path_times(task,3,0,times,0,false));
    assert(!tmpfs_stat(fixture.fs,inode.first_cluster,&stat) && stat.atime_sec==1700000000 && stat.mtime_sec==1700000000);
    assert(syscall_update_path_times(task,LINUX_AT_FDCWD,0,times,0,true)==-RELIEFOS_EFAULT);
    assert(fixture.lookups==lookups);
    const uint64_t calls[]={LINUX_SYS_UTIME,LINUX_SYS_UTIMES,LINUX_SYS_FUTIMESAT,LINUX_SYS_UTIMENSAT};
    task->fsuid=1000;
    for(unsigned call=0;call<4;++call) {
        struct storage_node original,replacement;
        assert(!tmpfs_create(fixture.fs,"/snapshot",LINUX_S_IFREG|0600,NULL,&original));
        struct reliefos_permissions snapshot_owner={0600,1000,1000};
        assert(!tmpfs_permissions(fixture.fs,original.first_cluster,&snapshot_owner,true));
        assert(!vfs_path_lookup(ns,&task->fs_paths.root,&task->fs_paths.cwd,"snapshot",0,&path));
        task_file_bind_path(file,&path);file->node=original;vfs_path_put(&path);
        int64_t raw[4]={111,123456,222,654321};
        if(!call)raw[1]=222;
        char name[]="snapshot";timestamp_name=(uintptr_t)name;timestamp_name_bytes=sizeof(name);
        timestamp_input=(uintptr_t)raw;timestamp_bytes=call ? sizeof(raw) : 2*sizeof(raw[0]);
        struct trap_frame frame={.rax=calls[call],.rdi=call>=2 ? 3 : (uintptr_t)name,
            .rsi=call>=2 ? 0 : (uintptr_t)raw,.rdx=call>=2 ? (uintptr_t)raw : 0};
        task_lookup_scope_begin(task,&frame);fixture.wait_update=true;
        assert(syscall_timestamp_object(task,frame.rax,frame.rdi,frame.rsi,frame.rdx,0)==-RELIEFOS_EAGAIN);
        assert(!tmpfs_unlink(fixture.fs,"/snapshot",false));
        assert(!tmpfs_create(fixture.fs,"/snapshot",LINUX_S_IFREG|0600,NULL,&replacement));
        assert(!tmpfs_permissions(fixture.fs,replacement.first_cluster,&snapshot_owner,true));
        assert(replacement.first_cluster!=original.first_cluster);
        assert(!vfs_path_lookup(ns,&task->fs_paths.root,&task->fs_paths.cwd,"snapshot",0,&path));
        task_file_bind_path(file,&path);file->node=replacement;vfs_path_put(&path);
        memset(raw,0x7f,sizeof(raw));timestamp_bytes=timestamp_name_bytes=0;
        task_lookup_scope_begin(task,&frame);
        assert(!syscall_timestamp_object(task,frame.rax,frame.rdi,frame.rsi,frame.rdx,0));
        assert(!tmpfs_stat(fixture.fs,original.first_cluster,&stat) && stat.atime_sec==111 && stat.mtime_sec==222);
        assert(!tmpfs_stat(fixture.fs,replacement.first_cluster,&stat) && stat.atime_sec==1700000000 && stat.mtime_sec==1700000000);
        task_lookup_scope_cancel(task);assert(!tmpfs_unlink(fixture.fs,"/snapshot",false));
    }
    vfs_path_put(&file->path_ref);sched_task_fs_release(task);free(task);
    assert(!vfs_namespace_destroy(ns));tmpfs_destroy(fixture.fs);
    puts("PASS futimens: actual retained/unlinked inode, no name replay, O_PATH/empty path, fsuid, CAP_FOWNER and NOW-only write permission");
}

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: metadata user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../../kernel/syscall/fs/metadata.c"
#include <reliefnt/swap.h>
static struct task current;
static struct task_file file;
static struct storage_node node;
static int64_t provider_size=123;
static struct vfs_namespace namespace;
static struct task_lookup_path result_slot;
static uint64_t swapped;
static bool contended,provider_owned,provider_completed,pending_once;
static unsigned callbacks,prepares,snapshots,releases;
static char snapshot[VFS_PATH_MAX],resolved[VFS_PATH_MAX],provider_result[32]="destination";
void *kernel_malloc(size_t n) { return malloc(n); }
void kernel_free(void *p) { free(p); }
bool user_range_writable(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped; }
bool user_range_ok(uint64_t address,uint64_t length) { return user_range_writable(address,length); }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && writing && length<=VFS_PATH_MAX);++prepares;
    if(address<4096)return -RELIEFOS_EFAULT;
    if(address==swapped){assert(!provider_owned);if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
void storage_release_task_io(uint32_t pid)
{ assert(pid==current.pid && provider_owned && provider_completed);provider_owned=provider_completed=false;++releases; }
int task_lookup_scope_next(struct task *task,struct task_lookup_path **out)
{ assert(task==&current);*out=&result_slot;return 0; }
int task_lookup_user_text(struct task *task,uint64_t address,uint32_t maximum,bool allow_null,struct task_user_text *out)
{
    assert(task==&current && maximum==VFS_PATH_MAX);
    if(!snapshots){assert(address || allow_null);snprintf(snapshot,sizeof(snapshot),"%s",address?(const char *)(uintptr_t)address:"");++snapshots;}
    *out=(struct task_user_text){.text=snapshot};return 0;
}
int resolve_user_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t fd,uint64_t address,
                                bool empty,bool real,uint32_t flags,struct vfs_lookup_result *out)
{
    assert(ns==&namespace && task==&current);(void)fd;(void)empty;(void)real;(void)flags;
    snprintf(resolved,sizeof(resolved),"%s",(const char *)(uintptr_t)address);*out=(struct vfs_lookup_result){0};return 0;
}
int resolve_kernel_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t fd,const char *text,
                                  bool empty,bool real,uint32_t flags,struct vfs_lookup_result *out)
{
    assert(ns==&namespace && task==&current);(void)fd;(void)empty;(void)real;(void)flags;
    snprintf(resolved,sizeof(resolved),"%s",text);*out=(struct vfs_lookup_result){0};return 0;
}
int vfs_path_readlink(const struct vfs_path *path,char *buffer,size_t capacity,size_t *length)
{
    (void)path;assert(capacity==VFS_PATH_MAX);++callbacks;provider_owned=true;
    if(pending_once){pending_once=false;return -RELIEFOS_EAGAIN;}
    provider_completed=true;*length=strlen(provider_result);memcpy(buffer,provider_result,*length);return 0;
}
void vfs_path_put(struct vfs_path *path) { (void)path; }
struct task_file *task_file_for_io(struct task *task,int fd)
{ assert(task==&current);return fd==3 ? &file : NULL; }
struct task_pty_fd *task_pty_for_io(struct task *task,int fd)
{ (void)task;(void)fd;return NULL; }
int task_pty_node_for_fd(struct task *task,int fd,struct storage_node *out,const char **path)
{ (void)task;(void)fd;(void)out;(void)path;return -RELIEFOS_EBADF; }
int task_pty_stream_for_fd(struct task *task,int fd)
{ (void)task;(void)fd;return -1; }
const struct storage_node *vfs_path_node(const struct vfs_path *path)
{ (void)path;return &node; }
int linux_stat_from_node(struct linux_stat_abi *out,const struct reliefos_stat *st,const struct storage_node *n)
{ (void)out;(void)st;(void)n;assert(0);return 0; }
void linux_stat_fd_type(struct linux_stat_abi *out,const struct task_file *f)
{ (void)out;assert(f==&file); }
int fs_virtual_permissions(const char *path,const struct storage_node *n,struct reliefos_permissions *out,bool write)
{ (void)path;(void)n;(void)out;(void)write;assert(0);return 0; }
int linux_stat_from_path(struct linux_stat_abi *out,const struct vfs_path *path)
{
    (void)path;++callbacks;provider_owned=true;
    if(pending_once){pending_once=false;return -RELIEFOS_EAGAIN;}
    provider_completed=true;*out=(struct linux_stat_abi){.st_size=provider_size};return 0;
}
int vfs_path_statfs(const struct vfs_path *path,struct linux_statfs_abi *out)
{ (void)path;(void)out;assert(0);return 0; }
void linux_statx_from_legacy(struct linux_statx *out,const struct linux_stat_abi *st,uint32_t mask)
{ (void)out;(void)st;(void)mask;assert(0); }
static void reset_fixture(void)
{
    free(result_slot.name);result_slot=(struct task_lookup_path){0};
    callbacks=prepares=snapshots=releases=0;swapped=0;contended=provider_owned=provider_completed=pending_once=false;
    strcpy(provider_result,"destination");provider_size=123;
}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=42;char name[32]="original",output[32]={0};
    pending_once=true;
    assert(syscall_readlink_object(&namespace,&current,-100,(uintptr_t)name,(uintptr_t)output,INT_MAX,false)==-RELIEFOS_EAGAIN);
    assert(provider_owned && !provider_completed && !prepares && !releases);
    swapped=(uintptr_t)output;strcpy(name,"replaced");
    assert(syscall_readlink_object(&namespace,&current,-100,(uintptr_t)name,(uintptr_t)output,INT_MAX,false)==11);
    assert(!provider_owned && releases==1 && !memcmp(output,"destination",11) && !strcmp(resolved,"original"));
    reset_fixture();strcpy(name,"original");memset(output,0,sizeof(output));
    swapped=(uintptr_t)output;contended=true;
    assert(syscall_readlink_object(&namespace,&current,-100,(uintptr_t)name,(uintptr_t)output,INT_MAX,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(callbacks==1 && releases==1 && snapshots==1 && swapped);
    strcpy(name,"replaced");strcpy(provider_result,"changed");contended=false;
    assert(syscall_readlink_object(&namespace,&current,-100,(uintptr_t)name,(uintptr_t)output,INT_MAX,false)==11);
    assert(!memcmp(output,"destination",11) && !strcmp(resolved,"original") && callbacks==1 && releases==1);
    reset_fixture();
    assert(syscall_readlink_object(&namespace,&current,-100,(uintptr_t)name,1,16,false)==-RELIEFOS_EFAULT);
    assert(callbacks==1 && releases==1); /* Preserve the original late output error. */
    reset_fixture();
    struct linux_stat_abi stat={0};pending_once=true;
    assert(syscall_metadata(&namespace,&current,LINUX_SYS_FSTAT,3,(uintptr_t)&stat,0,0,0)==-RELIEFOS_EAGAIN);
    assert(provider_owned && !prepares && !releases);
    swapped=(uintptr_t)&stat;
    assert(!syscall_metadata(&namespace,&current,LINUX_SYS_FSTAT,3,(uintptr_t)&stat,0,0,0));
    assert(stat.st_size==123 && releases==1 && !provider_owned);
    reset_fixture();stat=(struct linux_stat_abi){0};swapped=(uintptr_t)&stat;contended=true;
    assert(syscall_metadata(&namespace,&current,LINUX_SYS_FSTAT,3,(uintptr_t)&stat,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(callbacks==1 && releases==1 && !stat.st_size);
    contended=false;provider_size=999;
    assert(!syscall_metadata(&namespace,&current,LINUX_SYS_FSTAT,3,(uintptr_t)&stat,0,0,0));
    assert(stat.st_size==123 && callbacks==1 && releases==1);
    reset_fixture();puts("PASS metadata: pending owner, retained result, bounded swapped output and late EFAULT");
}

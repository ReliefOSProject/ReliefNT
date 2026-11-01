/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: getdents user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/syscall/io/directory.c"
#include <reliefnt/swap.h>
static struct task current;
static struct task_file file;
static unsigned allocations,reads,stats,releases,preparations;
static uint64_t blocked;
static bool contended,pending,stat_pending,provider_owned;
static unsigned entries=1;
static char first_name[16]="original";
void *kernel_malloc(size_t n){void *p=malloc(n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
struct task_file *task_file_for_io(struct task *task,int fd)
{assert(task==&current && fd==3);return &file;}
int file_can_read(const struct task_file *f){return f==&file;}
bool user_range_writable(uint64_t p,uint64_t n)
{return p>=4096 && n<=UINT64_MAX-p && !(contended && p==blocked);}
int swap_prepare_user_range(struct task *task,uint64_t p,uint64_t n,bool writing)
{assert(task==&current && n<=32768 && writing);++preparations;
 return contended && p==blocked ? SWAP_STORAGE_CONTENDED : 0;}
void storage_set_io_async_context(bool async){assert(!async);}
uint64_t time_uptime_us(void){return 1;}
int vfs_path_readdir(const struct vfs_path *path,uint64_t *cursor,struct reliefos_dir_entry *entry)
{(void)path;++reads;provider_owned=true;if(pending){pending=false;return -RELIEFOS_EAGAIN;}
 if(*cursor>=entries){provider_owned=false;return 0;}
 snprintf(entry->name,sizeof(entry->name),"%s",*cursor ? "second" : first_name);++*cursor;return 1;}
int vfs_path_dirent_stat(const struct vfs_path *path,const char *name,struct linux_stat_abi *out)
{(void)path;(void)name;++stats;provider_owned=true;
 if(stat_pending){stat_pending=false;return -RELIEFOS_EAGAIN;}
 *out=(struct linux_stat_abi){.st_ino=42,.st_mode=LINUX_S_IFREG};return 0;}
void storage_release_task_io(uint32_t pid){assert(pid==current.pid && provider_owned);provider_owned=false;++releases;}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);provider_owned=false;}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *task){(void)q;(void)task;assert(0);}
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;assert(0);}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;assert(0);return 0;}
void task_process_vm_scope_release(void *state){(void)state;assert(0);}
static int64_t call(char *buffer,uint32_t capacity)
{struct trap_frame frame={.rax=LINUX_SYS_GETDENTS64,.rdi=3,.rsi=(uintptr_t)buffer,.rdx=capacity};
 task_lookup_scope_begin(&current,&frame);
 return syscall_getdents_object(&current,LINUX_SYS_GETDENTS64,3,(uintptr_t)buffer,capacity);}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=7;current.syscall_file=&file;
    current.syscall_file_number=LINUX_SYS_GETDENTS64;file.node.type=RELIEFOS_FS_TYPE_DIR;
    char buffer[256]={0};blocked=(uintptr_t)buffer;pending=true;
    assert(call(buffer,sizeof(buffer))==-RELIEFOS_EAGAIN && provider_owned && !preparations);
    contended=true;assert(call(buffer,sizeof(buffer))==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!provider_owned && releases==1 && stats==1 && file.offset==0);
    strcpy(first_name,"changed");contended=false;
    int64_t n=call(buffer,sizeof(buffer));assert(n>0 && file.offset==1 && stats==1);
    assert(!strcmp(((struct linux_dirent64 *)buffer)->d_name,"original"));
    task_lookup_scope_cancel(&current);assert(!allocations);

    memset(&current.regular_io,0,sizeof(current.regular_io));file.offset=0;
    strcpy(first_name,"first");memset(buffer,0,sizeof(buffer));stat_pending=true;
    unsigned before_reads=reads,before_stats=stats;blocked=(uintptr_t)buffer;contended=false;
    assert(call(buffer,sizeof(buffer))==-RELIEFOS_EAGAIN && provider_owned);
    assert(reads==before_reads+1 && stats==before_stats+1);
    strcpy(first_name,"replacement");contended=true;
    assert(call(buffer,sizeof(buffer))==KERNEL_SYSCALL_SWAP_CONTENDED && !provider_owned);
    assert(reads==before_reads+1 && stats==before_stats+2);
    contended=false;assert(call(buffer,sizeof(buffer))>0);
    assert(!strcmp(((struct linux_dirent64 *)buffer)->d_name,"first"));
    task_lookup_scope_cancel(&current);assert(!allocations);

    memset(&current.regular_io,0,sizeof(current.regular_io));file.offset=0;entries=2;
    strcpy(first_name,"one");memset(buffer,0,sizeof(buffer));blocked=(uintptr_t)buffer+24;
    contended=true;n=call(buffer,sizeof(buffer));assert(n==24 && file.offset==1);
    assert(!strcmp(((struct linux_dirent64 *)buffer)->d_name,"one"));
    task_lookup_scope_cancel(&current);assert(!allocations);
    contended=false;memset(buffer,0,sizeof(buffer));
    assert(call(buffer,sizeof(buffer))>0 && file.offset==2);
    assert(!strcmp(((struct linux_dirent64 *)buffer)->d_name,"second"));
    task_lookup_scope_cancel(&current);assert(!allocations);
    puts("PASS getdents: pending owner, retained record, contention and committed prefix");
    return 0;
}

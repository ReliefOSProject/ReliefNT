/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm ioctl user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_drm_main
#include "drm_core_test.c"
#undef main
#include <reliefnt/swap.h>
static uint64_t blocked;
static bool block_write,contended;
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{assert(task && length<=32768);return contended && address==blocked && writing==block_write ? SWAP_STORAGE_CONTENDED:0;}
void storage_drain_task_io(uint32_t pid){(void)pid;}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;assert(0);}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void vfs_path_put(struct vfs_path *p){assert(!p->mount);}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;assert(0);}
int storage_inode_put(struct storage_inode_ref *i){
#ifdef TASK8_DRM_CLEANUP_TEST
 assert(!i);return 0;
#else
 (void)i;assert(0);return 0;
#endif
}
void task_process_vm_scope_release(void *s){(void)s;assert(0);}
static int64_t scoped_ioctl(struct task *t,struct task_file *f,uint32_t cmd,void *arg)
{struct trap_frame frame={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=cmd,.rdx=(uintptr_t)arg};
 task_lookup_scope_begin(t,&frame);return drm_file_ioctl(t,f,cmd,(uintptr_t)arg);}
#ifndef TASK8_DRM_NO_MAIN
int main(void)
{
    struct task task=make_task(500,false);task.kind=TASK_KIND_USER;task.state=TASK_READY;
    struct drm_master master={.next_magic=1};struct drm_file device={.master=&master};
    struct task_file file={.drm_file=&device,.character_file=(struct character_file *)1};task.syscall_file=&file;
    char name[16]={0},date[16]={0},replacement[16]={0};
    struct drm_version version={.name=name,.name_len=sizeof(name),.date=date,.date_len=sizeof(date)};
    blocked=(uintptr_t)date;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VERSION,&version)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!name[0] && !date[0]);version.name=replacement;
    contended=false;assert(!scoped_ioctl(&task,&file,DRM_IOCTL_VERSION,&version));
    assert(!strcmp(name,"reliefnt") && !strcmp(date,"0") && !replacement[0]);
    task_lookup_scope_cancel(&task);
    struct drm_auth auth={0};blocked=(uintptr_t)&auth;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_GET_MAGIC,&auth)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!device.magic && !master.magics);contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_GET_MAGIC,&auth) && auth.magic==1 && device.magic==1);
    task_lookup_scope_cancel(&task);kernel_free(master.magics);
    assert(!allocations);
    puts("PASS DRM ioctl: captured header, nested output wait and pre-effect outer output");
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm event front.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/futex.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>
#include <linux/uio.h>
#include "../../drivers/video/drm/drm_internal.h"
#include "../../drivers/video/drm/file_ops.h"
static struct task current;
static struct drm_file drm;
static struct task_file file;
static uint64_t vector_address;
static bool metadata_contended;
static unsigned payload_touches,blocks,allocations;
struct task *sched_current_task(void){return &current;}
struct task_file *task_file_for_io(struct task *t,int fd){assert(t==&current && fd==3);return &file;}
struct task_pty_fd *task_pty_for_io(struct task *t,int fd){(void)t;(void)fd;return NULL;}
int file_can_read(const struct task_file *f){return !(f->flags&RELIEFOS_O_WRONLY);}
int file_can_write(const struct task_file *f){return (f->flags&RELIEFOS_O_ACCMODE)!=RELIEFOS_O_RDONLY;}
int task_device_is(const struct task_file *f,uint32_t kind){(void)f;(void)kind;return 0;}
void *kernel_malloc(size_t n){void *p=malloc(n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
bool user_range_ok(uint64_t p,uint64_t n){if(p==vector_address && n<=1024*sizeof(struct iovec))return true;
    ++payload_touches;return false;}
bool user_range_writable(uint64_t p,uint64_t n){(void)p;(void)n;++payload_touches;return false;}
int swap_prepare_user_range(struct task *t,uint64_t p,uint64_t n,bool write){assert(t==&current && n<=32768);
    if(p!=vector_address){++payload_touches;return SWAP_STORAGE_CONTENDED;}
    assert(!write);return metadata_contended ? SWAP_STORAGE_CONTENDED : 0;}
bool character_file_has_readv(const struct character_file *h){(void)h;return true;}
int64_t character_file_read(struct character_file *h,struct task *t,struct task_file *f,void *p,uint64_t n)
{(void)h;return drm_event_read(t,f,(uintptr_t)p,n);}
int64_t character_file_readv(struct character_file *h,struct task *t,struct task_file *f,
 const struct iovec *v,uint64_t n,bool pos,uint64_t off,uint64_t flags)
{(void)h;return drm_event_readv(t,f,v,n,pos,off,flags);}
int kernel_wait_queue_add(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;return 0;}
void sched_block_current(void){++blocks;}
void sched_sleep_current_until(uint64_t t){(void)t;assert(0);}
uint64_t time_ticks(void){return 1;}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;assert(0);}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void vfs_path_put(struct vfs_path *p){assert(!p->mount);}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;assert(0);}
int storage_inode_put(struct storage_inode_ref *i){(void)i;assert(0);return 0;}
void task_process_vm_scope_release(void *s){(void)s;assert(0);}
int64_t syscall_io_read(uint64_t,uint64_t,uint64_t);
int64_t syscall_vector_io(struct task *,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static int64_t readv_scoped(struct iovec *v)
{struct trap_frame f={.rax=LINUX_SYS_READV,.rdi=3,.rsi=(uintptr_t)v,.rdx=2};
 task_lookup_scope_begin(&current,&f);return syscall_vector_io(&current,LINUX_SYS_READV,3,(uintptr_t)v,2,0,0,0);}
int main(void)
{
    current=(struct task){.pid=91,.kind=TASK_KIND_USER,.state=TASK_READY,.syscall_file=&file};
    file=(struct task_file){.drm_file=&drm,.character_file=(struct character_file *)1,.flags=RELIEFOS_O_NONBLOCK};
    uint64_t absent=RELIEFNT_USER_BASE+4096;
    assert(syscall_io_read(3,absent,65536)==-LINUX_EAGAIN && !payload_touches);
    assert(syscall_io_read(3,RELIEFNT_USER_TOP-1,2)==-LINUX_EFAULT && !payload_touches);
    struct iovec v[2]={{(void *)(uintptr_t)absent,8},{(void *)(uintptr_t)(absent+4096),16}};
    vector_address=(uintptr_t)v;metadata_contended=true;
    assert(readv_scoped(v)==KERNEL_SYSCALL_SWAP_CONTENDED && !payload_touches);
    metadata_contended=false;file.flags=0;assert(readv_scoped(v)==KERNEL_SYSCALL_BLOCKED && blocks==1);
    v[0].iov_base=(void *)(uintptr_t)(RELIEFNT_USER_TOP-1);v[0].iov_len=8;
    file.flags=RELIEFOS_O_NONBLOCK;assert(readv_scoped(v)==-LINUX_EAGAIN && !payload_touches);
    task_lookup_scope_cancel(&current);
    assert(readv_scoped(v)==-LINUX_EFAULT && !payload_touches);
    task_lookup_scope_cancel(&current);assert(!allocations);
    puts("PASS DRM real scalar/vector fronts: no unused page touch, stable iovec snapshot");
}

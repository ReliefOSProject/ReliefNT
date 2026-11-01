/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: scalar user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>
#include <reliefnt/paging.h>
#include <reliefnt/wait.h>
#include <linux/errno.h>
static struct task current;
static struct task_file file;
static struct kernel_wait_queue queue;
static unsigned char bytes[65536];
static uint64_t swapped;
static bool contended;
static unsigned prepares,removed,writes,reads;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task,int fd)
{ assert(task==&current && fd==3);return &file; }
struct task_pty_fd *task_pty_for_io(struct task *task,int fd)
{ assert(task==&current && fd==3);return NULL; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing;assert(task==&current && length<=32768);++prepares;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=32768 && address!=swapped; }
bool user_range_writable(uint64_t address,uint64_t length)
{ return user_range_ok(address,length); }
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *task)
{ assert(q==&queue && task==&current);task->waiting_queue=NULL;++removed; }
int task_pipe_write(struct task_file *f,const void *buffer,uint32_t length)
{ assert(f==&file && buffer==bytes && length<=32768);++writes;return length; }
int task_pipe_read(struct task_file *f,void *buffer,uint32_t length)
{ assert(f==&file && buffer==bytes && length<=32768);++reads;memset(buffer,0x37,length);return length; }
void kernel_poll_readiness_notify(void) { }
int file_can_read(const struct task_file *f) { assert(f==&file);return 1; }
int file_can_write(const struct task_file *f) { assert(f==&file);return 1; }
int task_device_is(const struct task_file *f,uint32_t kind)
{ return f && (f->flags&TASK_FILE_FLAG_DEV_NODE) && f->node.first_cluster==kind; }
int64_t syscall_io_write(uint64_t fd,uint64_t buffer,uint64_t count);
int64_t syscall_io_read(uint64_t fd,uint64_t buffer,uint64_t count);
int main(void)
{
    current.kind=TASK_KIND_USER;current.syscall_file=&file;file.flags=TASK_FILE_FLAG_PIPE;
    swapped=(uintptr_t)bytes;contended=true;current.waiting_queue=&queue;
    assert(syscall_io_write(3,(uintptr_t)bytes,sizeof(bytes))==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!writes && removed==1 && !current.waiting_queue);
    contended=false;assert(syscall_io_write(3,(uintptr_t)bytes,sizeof(bytes))==32768);
    assert(writes==1 && !swapped);
    assert(syscall_io_write(3,(uintptr_t)bytes,4096)==4096 && writes==2);
    swapped=(uintptr_t)bytes;contended=true;
    assert(syscall_io_read(3,(uintptr_t)bytes,sizeof(bytes))==KERNEL_SYSCALL_SWAP_CONTENDED && !reads);
    contended=false;assert(syscall_io_read(3,(uintptr_t)bytes,sizeof(bytes))==32768);
    assert(reads==1 && bytes[0]==0x37 && prepares);
    file.flags=TASK_FILE_FLAG_EVENTFD;file.aux=4;uint64_t value=0;
    swapped=(uintptr_t)&value;contended=true;
    assert(syscall_io_read(3,(uintptr_t)&value,8)==KERNEL_SYSCALL_SWAP_CONTENDED && file.aux==4);
    contended=false;assert(syscall_io_read(3,(uintptr_t)&value,8)==8 && value==4 && !file.aux);
    file.flags=TASK_FILE_FLAG_DEV_NULL;
    unsigned before=prepares;
    uint64_t unmapped=RELIEFNT_USER_BASE+0x1000;
    assert(syscall_io_write(3,unmapped,32)==32 && prepares==before);
    assert(syscall_io_read(3,unmapped,32)==0 && prepares==before);
    assert(syscall_io_write(3,unmapped,UINT64_MAX)==-LINUX_EFAULT && prepares==before);
    assert(syscall_io_read(3,RELIEFNT_USER_TOP-8,16)==-LINUX_EFAULT && prepares==before);
    file.flags=TASK_FILE_FLAG_PIPE;
    assert(syscall_io_write(3,(uintptr_t)bytes,UINT64_MAX)==-LINUX_EFAULT && prepares==before);
    assert(syscall_io_read(3,RELIEFNT_USER_TOP-8,16)==-LINUX_EFAULT && prepares==before);
    puts("PASS scalar pipe/eventfd/null: bounded swapped slices and retained wait progress");
}

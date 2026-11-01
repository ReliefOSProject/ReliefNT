/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: tty ioctl.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/device/legacy_ioctl.c"
#include <reliefnt/swap.h>
static struct task current;
static struct task_pty_fd endpoint = {.used=1,.pty_id=1,.endpoint=TASK_PTY_ENDPOINT_SLAVE};
static bool contended;
static unsigned preparations, changes, flushes;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task,int fd) {(void)task;(void)fd;return NULL;}
struct task_file *task_file_for_fd(struct task *task,int fd) {(void)task;(void)fd;return NULL;}
struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task,int fd)
{assert(task==&current && fd==3);return &endpoint;}
struct task_pty_fd *task_pty_for_io(struct task *task,int fd)
{return task_pty_endpoint_for_fd(task,fd);}
int task_pty_stream_for_fd(struct task *task,int fd) {(void)task;(void)fd;return -1;}
int task_evdev_ioctl(struct task_file *file,uint64_t request,uint64_t argument)
{(void)file;(void)request;(void)argument;return -RELIEFOS_ENOTTY;}
int task_block_device(const struct task_file *file,uint32_t *disk,int32_t *partition)
{(void)file;(void)disk;(void)partition;return 0;}
int task_device_is(const struct task_file *file,uint32_t kind) {(void)file;(void)kind;return 0;}
uint32_t pty_vt_number(uint32_t id) {return id;}
int pty_is_active(uint32_t id) {assert(id==1);return 1;}
int pty_is_hungup(uint32_t id) {assert(id==1);return 0;}
int64_t pty_check_change(uint32_t id,uint32_t pid,int sig)
{assert(id==1 && pid==1 && sig==22);return 0;}
int pty_get_termios(uint32_t id,struct reliefos_pty_termios *value)
{assert(id==1);memset(value,0,sizeof(*value));return 0;}
int pty_set_termios(uint32_t id,const struct reliefos_pty_termios *value)
{assert(id==1 && value);++changes;return 0;}
void pty_flush_input(uint32_t id) {assert(id==1);++flushes;}
int pty_vt_get_mode(uint32_t id,struct vt_mode *mode)
{assert(id==1);*mode=(struct vt_mode){.mode=VT_AUTO};return 0;}
bool user_range_ok(uint64_t p,uint64_t n) {return p>=4096 && n<=UINT64_MAX-p;}
bool user_range_writable(uint64_t p,uint64_t n) {return user_range_ok(p,n);}
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{assert(task==&current && address>=4096 && size<=32768);(void)writing;++preparations;
 return contended ? SWAP_STORAGE_CONTENDED : 0;}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=1;current.controlling_pty_id=1;
    struct linux_termios termios={0};
    contended=true;
    assert(syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,TCSETSF,(uintptr_t)&termios,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!changes && !flushes);
    contended=false;
    assert(!syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,TCSETSF,(uintptr_t)&termios,0,0,0));
    assert(changes==1 && flushes==1);
    struct vt_mode mode={0};contended=true;
    assert(syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,VT_GETMODE,(uintptr_t)&mode,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;
    assert(!syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,VT_GETMODE,(uintptr_t)&mode,0,0,0));
    unsigned before=preparations;
    assert(syscall_legacy_ioctl(LINUX_SYS_IOCTL,3,0x7fff1234,1,0,0,0)==-RELIEFOS_ENOTTY);
    assert(preparations==before);
    puts("PASS VT/TTY: swapped fixed operands, setter side effects and ignored unknown ioctl");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: exec continuation.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
/* Link the production exec continuation with controlled scheduler/loader edges. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/sched.h>
#include <reliefnt/userland.h>
#include <reliefnt/futex.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/console.h>
#include <linux/errno.h>

static struct task target, *prepared_owner;
static struct futex_exit_cursor cursor;
static int cleanup_status;
static unsigned cleanup_calls, commits, destroys, fs_releases, exits;
struct task *sched_exec_staged(const struct task *task) { assert(task==&target); return prepared_owner; }
struct task *sched_exec_take(struct task *task)
{ assert(task==&target);struct task *p=prepared_owner;prepared_owner=NULL;return p; }
struct futex_exit_cursor *sched_futex_exit_cursor(struct task *task) { assert(task==&target);return &cursor; }
int sched_prepare_exec_current(struct task *task) { assert(task==&target);++cleanup_calls;return cleanup_status; }
void sched_exec_replace_mm(struct task *task,const struct address_space *replacement)
{ assert(task==&target && replacement->cr3==0x88000);++commits;task->shared_mm=NULL; }
void svga_gpu_release_owner(uint32_t pid) { assert(pid==target.pid); }
void kernel_free(void *p) { free(p); }
void address_space_destroy(struct address_space *as) { assert(as);++destroys; }
void sched_task_vma_release(struct task *task) { assert(task); }
void sched_task_fs_release(struct task *task) { assert(task);++fs_releases; }
void kernel_signal_reset_handlers(struct task *task) { assert(task==&target); }
void syscall_close_cloexec_files(struct task *task) { assert(task==&target); }
void arch_fpu_task_init(void *state) { assert(state==target.fpu_state); }
void arch_fpu_restore(const void *state) { assert(state==target.fpu_state); }
void kernel_debug_finish_initramfs_boot(struct task *task) { assert(task==&target); }
void sched_exit(uint32_t pid,uint64_t status) { assert(pid==target.pid && status==127);++exits;target.state=TASK_EXITED; }
void console_log(enum console_log_level level,const char *format,...)
{ (void)level;(void)format; }

static struct task *make_prepared(const char *text)
{
    struct task *p=calloc(1,sizeof(*p));assert(p);
    p->as.cr3=0x88000;p->frame.rip=0x4444000;p->frame.rsp=0x7777000;
    strcpy(p->path,"/held/image");strcpy(p->name_storage,"held");
    p->exec_data=malloc(strlen(text)+1);p->exec_argv=malloc(2*sizeof(char *));
    p->exec_envp=malloc(sizeof(char *));assert(p->exec_data && p->exec_argv && p->exec_envp);
    strcpy(p->exec_data,text);p->exec_argv[0]=p->exec_data;p->exec_argv[1]=NULL;
    p->exec_envp[0]=NULL;p->exec_argc=1;p->exec_data_len=strlen(text)+1;
    return p;
}
int main(void)
{
    target.kind=TASK_KIND_USER;target.pid=55;target.state=TASK_RUNNING;
    target.as.cr3=0x11000;target.frame.rip=0x1111000;
    target.exec_data=malloc(4);target.exec_argv=malloc(sizeof(char *));target.exec_envp=malloc(sizeof(char *));
    prepared_owner=make_prepared("original-argument");char *snapshot=prepared_owner->exec_data;
    cursor.phase=3;cleanup_status=-EAGAIN;
    assert(userland_exec_continue(&target)==KERNEL_SYSCALL_EXEC_COMMIT_WAIT);
    assert(prepared_owner && target.as.cr3==0x11000 && target.frame.rip==0x1111000 && !commits);
    char mutable_user_input[]="replacement";mutable_user_input[0]='X';
    assert(!strcmp(prepared_owner->exec_data,"original-argument"));
    cleanup_status=0;
    assert(!userland_exec_continue(&target));
    assert(!prepared_owner && commits==1 && target.as.cr3==0x88000 && target.frame.rip==0x4444000);
    assert(target.exec_data==snapshot && !strcmp(target.exec_argv[0],"original-argument"));
    assert(!strcmp(target.path,"/held/image") && !cursor.phase && fs_releases==1);
    free(target.exec_data);free(target.exec_argv);free(target.exec_envp);
    target.exec_data=NULL;target.exec_argv=target.exec_envp=NULL;
    prepared_owner=make_prepared("fatal");cleanup_status=-EIO;
    assert(userland_exec_continue(&target)==KERNEL_SYSCALL_EXEC_FATAL);
    assert(!prepared_owner && exits==1 && destroys==1 && fs_releases==2);
    assert(target.state==TASK_EXITED && cleanup_calls==3);
    puts("PASS owned exec continuation: wait retains prepared image/argv, commit transfers once, fatal cleanup");
}

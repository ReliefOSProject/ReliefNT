/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Preserve admitted fixed process inputs across output contention.
 * User mappings and scheduling are controlled; process and lookup code are real.
 */
#define main task8_inherited_process_user_main
#include "task8_process_user_test.c"
#undef main
#include <stdlib.h>

static unsigned allocations,frees;
void *kernel_malloc(size_t size){void *p=malloc(size);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){++frees;free(p);}}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void task_process_vm_scope_release(void *state){(void)state;abort();}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{ (void)queue;(void)task;abort(); }
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;abort();}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;abort();}

int main(int argc,char **argv)
{
    assert(argc==2);current.kind=TASK_KIND_USER;current.pid=42;
    if(!strcmp(argv[1],"mask-overlap") || !strcmp(argv[1],"limit-overlap")) {
        static uint8_t bytes[8192] __attribute__((aligned(4096)));
        bool mask=!strcmp(argv[1],"mask-overlap");
        uint64_t in=(uintptr_t)bytes+4096-(mask ? 8 : 16);
        uint64_t out=(uintptr_t)bytes+4096-(mask ? 4 : 8);
        struct trap_frame f={.rax=mask ? LINUX_SYS_RT_SIGPROCMASK : LINUX_SYS_PRLIMIT64};
        if(mask) {
            uint64_t value=4;memcpy((void *)(uintptr_t)in,&value,sizeof(value));
            f.rdi=0;f.rsi=in;f.rdx=out;f.r10=8;
        } else {
            struct linux_rlimit64 value={5,10};memcpy((void *)(uintptr_t)in,&value,sizeof(value));
            current.limits.core=(struct linux_rlimit64){2,10};
            f.rdi=0;f.rsi=LINUX_RLIMIT_CORE;f.rdx=in;f.r10=out;
        }
        swapped=out;contended=true;task_lookup_scope_begin(&current,&f);
        int64_t rc=mask ? syscall_linux_signal(f.rax,f.rdi,f.rsi,f.rdx,f.r10,0)
                        : process_resource_limit(f.rax,f.rdi,f.rsi,f.rdx,f.r10);
        assert(rc==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(mask ? !current.blocked_signals : current.limits.core.rlim_cur==2);
        memset((void *)(uintptr_t)in,0x7f,mask ? 8 : 16);unmapped=in;contended=false;
        task_lookup_scope_begin(&current,&f);
        rc=mask ? syscall_linux_signal(f.rax,f.rdi,f.rsi,f.rdx,f.r10,0)
                : process_resource_limit(f.rax,f.rdi,f.rsi,f.rdx,f.r10);
        assert(!rc);
        if(mask) {
            uint64_t old;memcpy(&old,(void *)(uintptr_t)out,sizeof(old));
            assert(!old && current.blocked_signals==4);
        } else {
            struct linux_rlimit64 old;memcpy(&old,(void *)(uintptr_t)out,sizeof(old));
            assert(old.rlim_cur==2 && current.limits.core.rlim_cur==5);
        }
    } else if(!strcmp(argv[1],"permission")) {
        struct task target={.pid=43,.kind=TASK_KIND_USER,.uid=123,.euid=123,.suid=123};
        struct linux_rlimit64 input={5,10},old={0};external_task=&target;
        target.limits.core=(struct linux_rlimit64){2,10};
        struct trap_frame f={.rax=LINUX_SYS_PRLIMIT64,.rdi=43,.rsi=LINUX_RLIMIT_CORE,
                             .rdx=(uintptr_t)&input,.r10=(uintptr_t)&old};
        swapped=(uintptr_t)&old;contended=true;task_lookup_scope_begin(&current,&f);
        assert(process_resource_limit(f.rax,43,f.rsi,f.rdx,f.r10)==-LINUX_EPERM);
        assert(prepares==1 && target.limits.core.rlim_cur==2);
        task_lookup_scope_cancel(&current);external_task=NULL;
    } else if(!strcmp(argv[1],"mask")) {
        uint64_t input=4,old=0;
        struct trap_frame f={.rax=LINUX_SYS_RT_SIGPROCMASK,.rdi=0,.rsi=(uintptr_t)&input,
                             .rdx=(uintptr_t)&old,.r10=8};
        swapped=(uintptr_t)&old;contended=true;task_lookup_scope_begin(&current,&f);
        assert(syscall_linux_signal(f.rax,0,f.rsi,f.rdx,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
        input=8;unmapped=(uintptr_t)&input;contended=false;task_lookup_scope_begin(&current,&f);
        assert(!syscall_linux_signal(f.rax,0,f.rsi,f.rdx,8,0));
        assert(current.blocked_signals==4 && !old);
    } else if(!strcmp(argv[1],"limit")) {
        struct linux_rlimit64 input={5,10},old={0};current.limits.core=(struct linux_rlimit64){2,10};
        struct trap_frame f={.rax=LINUX_SYS_PRLIMIT64,.rdi=0,.rsi=LINUX_RLIMIT_CORE,
                             .rdx=(uintptr_t)&input,.r10=(uintptr_t)&old};
        swapped=(uintptr_t)&old;contended=true;task_lookup_scope_begin(&current,&f);
        assert(process_resource_limit(f.rax,0,f.rsi,f.rdx,f.r10)==KERNEL_SYSCALL_SWAP_CONTENDED);
        input.rlim_cur=8;unmapped=(uintptr_t)&input;contended=false;task_lookup_scope_begin(&current,&f);
        assert(!process_resource_limit(f.rax,0,f.rsi,f.rdx,f.r10));
        assert(current.limits.core.rlim_cur==5 && old.rlim_cur==2);
    } else {
        assert(!strcmp(argv[1],"reuse") || !strcmp(argv[1],"live") ||
               !strcmp(argv[1],"cancel") || !strcmp(argv[1],"reauthorize"));
        struct task target={.pid=43,.kind=TASK_KIND_USER},replacement={.pid=43,.kind=TASK_KIND_USER};
        target.limits.core=(struct linux_rlimit64){2,10};replacement.limits.core=(struct linux_rlimit64){3,10};
        struct linux_rlimit64 input={5,10},old={0};external_task=&target;
        struct trap_frame f={.rax=LINUX_SYS_PRLIMIT64,.rdi=43,.rsi=LINUX_RLIMIT_CORE,
                             .rdx=(uintptr_t)&input,.r10=(uintptr_t)&old};
        swapped=(uintptr_t)&old;contended=true;task_lookup_scope_begin(&current,&f);
        assert(process_resource_limit(f.rax,43,f.rsi,f.rdx,f.r10)==KERNEL_SYSCALL_SWAP_CONTENDED);
        if(!strcmp(argv[1],"reuse")) {
            sched_task_identity_detach(&target);external_task=&replacement;
            contended=false;task_lookup_scope_begin(&current,&f);
            assert(process_resource_limit(f.rax,43,f.rsi,f.rdx,f.r10)==-LINUX_ESRCH);
            assert(replacement.limits.core.rlim_cur==3);
        } else if(!strcmp(argv[1],"live")) {
            contended=false;task_lookup_scope_begin(&current,&f);
            assert(!process_resource_limit(f.rax,43,f.rsi,f.rdx,f.r10));
            assert(target.limits.core.rlim_cur==5 && old.rlim_cur==2);
        } else if(!strcmp(argv[1],"reauthorize")) {
            target.uid=target.euid=target.suid=123;contended=false;
            task_lookup_scope_begin(&current,&f);
            assert(process_resource_limit(f.rax,43,f.rsi,f.rdx,f.r10)==-LINUX_EPERM);
            assert(target.limits.core.rlim_cur==2 && !old.rlim_cur);
        }
        task_lookup_scope_cancel(&current);sched_task_identity_detach(&target);external_task=NULL;
    }
    task_lookup_scope_cancel(&current);assert(allocations==frees);
    puts("PASS fixed process input survives retry and scope cancellation");return 0;
}

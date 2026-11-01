/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Keep argv/envp words and string prefixes through swap contention.
 * The exec importer and lookup scope are real; user-memory faults are modeled.
 */
#define TASK8_EXEC_SNAPSHOT_TEST 1
#define main task8_inherited_exec_script_main
#include "exec_script_test.c"
#undef main

int main(int argc,char **argv)
{
    assert(argc==2);caller.kind=TASK_KIND_USER;caller.pid=42;
    caller.limits.stack.rlim_cur=8u*1024u*1024u;
    static char first[4096] __attribute__((aligned(4096)))="first";
    static char cross[8192] __attribute__((aligned(4096)));
    char *arguments[]={first,"second",NULL};char *environment[]={"KEY=original",NULL};
    if(!strcmp(argv[1],"limits")) {
        char *long_argument=malloc(SCHED_EXEC_STR_MAX+1);assert(long_argument);
        memset(long_argument,'a',SCHED_EXEC_STR_MAX+1);long_argument[SCHED_EXEC_STR_MAX-1]=0;
        char *vector[]={long_argument,NULL};
        struct trap_frame f={.rax=LINUX_SYS_EXECVE,.rdi=(uintptr_t)"/x",.rsi=(uintptr_t)vector};
        struct exec_params_kernel params={0};task_lookup_scope_begin(&caller,&f);
        assert(!copy_exec_params_from_user(f.rsi,0,&params) && params.data_len==SCHED_EXEC_STR_MAX);
        free_exec_params(&params);task_lookup_scope_cancel(&caller);
        long_argument[SCHED_EXEC_STR_MAX-1]='a';long_argument[SCHED_EXEC_STR_MAX]=0;
        task_lookup_scope_begin(&caller,&f);
        assert(copy_exec_params_from_user(f.rsi,0,&params)==-RELIEFOS_E2BIG);
        task_lookup_scope_cancel(&caller);
        long_argument[65535]=0;char *many[40];
        for(unsigned i=0;i<39;++i)many[i]=long_argument;many[39]=NULL;
        f.rsi=(uintptr_t)many;task_lookup_scope_begin(&caller,&f);
        assert(copy_exec_params_from_user(f.rsi,0,&params)==-RELIEFOS_E2BIG);
        task_lookup_scope_cancel(&caller);free(long_argument);
        assert(exec_allocations==exec_frees);
        puts("PASS scoped exec per-string and aggregate limits");return 0;
    }
    if(!strcmp(argv[1],"cancel") || !strcmp(argv[1],"oom")) {
        static char long_argument[8192] __attribute__((aligned(4096)));
        memset(long_argument,'q',sizeof(long_argument));long_argument[sizeof(long_argument)-1]=0;
        arguments[1]=long_argument;exec_blocked_address=(uintptr_t)&arguments[1];
        struct trap_frame f={.rax=LINUX_SYS_EXECVE,.rdi=(uintptr_t)"/x",
                             .rsi=(uintptr_t)arguments,.rdx=(uintptr_t)environment};
        struct exec_params_kernel params={0};task_lookup_scope_begin(&caller,&f);exec_contended=true;
        assert(copy_exec_params_from_user(f.rsi,f.rdx,&params)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(exec_allocations>exec_frees && !params.data);
        if(!strcmp(argv[1],"oom")) {
            exec_contended=false;fail_allocation=true;task_lookup_scope_begin(&caller,&f);
            assert(copy_exec_params_from_user(f.rsi,f.rdx,&params)==-RELIEFOS_ENOMEM);
            fail_allocation=false;
        }
        task_lookup_scope_cancel(&caller);task_lookup_scope_cancel(&caller);
        assert(exec_allocations==exec_frees && !params.data);
        puts("PASS scoped exec retained import cancellation/OOM cleanup");return 0;
    }
    if(!strcmp(argv[1],"path")) {
        static char pathname[64]="/original";
        struct trap_frame f={.rax=LINUX_SYS_EXECVE,.rdi=(uintptr_t)pathname,
                             .rsi=(uintptr_t)arguments,.rdx=(uintptr_t)environment};
        struct exec_params_kernel params={0};struct task_user_text path={0};
        struct task_lookup_path *resolver_slot=NULL;
        exec_blocked_address=(uintptr_t)&arguments[1];exec_contended=true;
        task_lookup_scope_begin(&caller,&f);
        assert(!task_lookup_user_text(&caller,f.rdi,VFS_PATH_MAX,false,&path));
        assert(!strcmp(path.text,"/original"));
        /* The real object resolver takes slot 1 after text slot 0. This
         * fixture models its already-completed provider and consumes that
         * slot, so argv must use the dynamically extended slot 2. */
        assert(!task_lookup_scope_next(&caller,&resolver_slot) && resolver_slot);
        assert(copy_exec_params_from_user(f.rsi,f.rdx,&params)==KERNEL_SYSCALL_SWAP_CONTENDED);
        strcpy(pathname,"/changed");strcpy(first,"other");arguments[0]=(char *)1;
        exec_unmapped_address=(uintptr_t)pathname;exec_contended=false;
        task_lookup_scope_begin(&caller,&f);
        assert(!task_lookup_user_text(&caller,f.rdi,VFS_PATH_MAX,false,&path));
        assert(!strcmp(path.text,"/original"));
        struct task_lookup_path *retry_slot=NULL;
        assert(!task_lookup_scope_next(&caller,&retry_slot) && retry_slot==resolver_slot);
        assert(!copy_exec_params_from_user(f.rsi,f.rdx,&params));
        assert(!strcmp(params.argv[0],"first") && !strcmp(params.envp[0],"KEY=original"));
        free_exec_params(&params);task_lookup_scope_cancel(&caller);
        assert(exec_allocations==exec_frees);
        puts("PASS exec pathname, modeled resolver slot and argv retain accepted content");return 0;
    }
    if(!strcmp(argv[1],"string")) {
        memcpy(cross+4092,"prefix-tail",12);arguments[0]=cross+4092;
        exec_blocked_address=(uintptr_t)cross+4096;
    } else if(!strcmp(argv[1],"env"))exec_blocked_address=(uintptr_t)&environment[0];
    else {assert(!strcmp(argv[1],"arg"));exec_blocked_address=(uintptr_t)&arguments[1];}
    struct trap_frame f={.rax=LINUX_SYS_EXECVE,.rdi=(uintptr_t)"/x",
                         .rsi=(uintptr_t)arguments,.rdx=(uintptr_t)environment};
    struct exec_params_kernel params={0};task_lookup_scope_begin(&caller,&f);exec_contended=true;
    assert(copy_exec_params_from_user(f.rsi,f.rdx,&params)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!params.argv && !params.envp && !params.data);
    if(!strcmp(argv[1],"string")) {
        memcpy(cross+4092,"xxxx",4);exec_unmapped_address=(uintptr_t)cross+4092;
    } else {
        memcpy(first,"other",6);exec_unmapped_address=(uintptr_t)first;
        arguments[0]=(char *)1;
    }
    exec_contended=false;task_lookup_scope_begin(&caller,&f);
    assert(!copy_exec_params_from_user(f.rsi,f.rdx,&params));
    assert(!strcmp(params.argv[0],!strcmp(argv[1],"string") ? "prefix-tail" : "first"));
    assert(!strcmp(params.argv[1],"second") && !strcmp(params.envp[0],"KEY=original"));
    free_exec_params(&params);task_lookup_scope_cancel(&caller);assert(exec_allocations==exec_frees);
    puts("PASS exec argv/env snapshots preserve observed words and strings");return 0;
}

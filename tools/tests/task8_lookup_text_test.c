/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Resume pathname and exec-text imports after page-boundary contention.
 * The path and scope code are production; VFS walk and page availability are
 * controlled so accepted-prefix mutation and cancellation are observable.
 */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/internal.h"
#include <reliefnt/swap.h>

static struct task current;
static unsigned char memory[65536] __attribute__((aligned(4096)));
static uint64_t blocked,unmapped;
static bool contended,lazy_file;
static unsigned allocations,frees,walks;
static char observed[64];
void *kernel_malloc(size_t size){void *p=malloc(size);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){++frees;free(p);}}
struct task *sched_current_task(void){return &current;}
#ifdef CONFIG_SWAP
int swap_prepare_user_range(struct task *t,uint64_t p,uint64_t n,bool writing)
{
    assert(t==&current && !writing && n<=32768);
    if((p&~4095ULL)==unmapped)return -RELIEFOS_EFAULT;
    return !lazy_file && p==blocked && contended ? SWAP_STORAGE_CONTENDED : 0;
}
#endif
bool user_range_ok(uint64_t p,uint64_t n)
{ return p>=4096 && n<=UINT64_MAX-p && (p&~4095ULL)!=unmapped; }
int user_range_readable(uint64_t p,uint64_t n)
{
    if(!user_range_ok(p,n))return -RELIEFOS_EFAULT;
    return lazy_file && p==blocked && contended ? -RELIEFOS_EAGAIN : 0;
}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void task_process_vm_scope_release(void *state){(void)state;abort();}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;abort();}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;abort();}
int storage_inode_put(struct storage_inode_ref *i){(void)i;abort();}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void vfs_path_put(struct vfs_path *p){*p=(struct vfs_path){0};}
void vfs_path_get(const struct vfs_path *p,struct vfs_path *out){*out=*p;}
const struct storage_node *vfs_path_node(const struct vfs_path *p)
{ static struct storage_node n={.type=RELIEFOS_FS_TYPE_DIR};return p && p->mount ? &n : NULL; }
struct task_file *task_file_for_io(struct task *task,int fd){(void)task;(void)fd;abort();}
struct task_pty_fd *task_pty_for_io(struct task *task,int fd){(void)task;(void)fd;abort();}
int fs_permissions_check_path(const struct task *t,const struct vfs_path *p,uint32_t a,bool real)
{ (void)t;(void)p;(void)a;(void)real;abort(); }
int vfs_path_resolve_resume(struct vfs_namespace *ns,const struct vfs_path *root,
                            const struct vfs_path *start,const char *name,
                            const struct vfs_lookup_policy *policy,struct vfs_walk **state,
                            struct vfs_lookup_result *out)
{
    (void)ns;(void)start;(void)policy;assert(!*state);++walks;
    snprintf(observed,sizeof(observed),"%s",name);out->path=*root;return 0;
}
int vfs_path_resolve(struct vfs_namespace *ns,const struct vfs_path *root,
                     const struct vfs_path *start,const char *name,
                     const struct vfs_lookup_policy *policy,struct vfs_lookup_result *out)
{ struct vfs_walk *state=NULL;return vfs_path_resolve_resume(ns,root,start,name,policy,&state,out); }

static unsigned first_backing(void)
{
#ifdef CONFIG_SWAP
    return 0;
#else
    return 1;
#endif
}

static void test_bytes(void)
{
    const uint32_t length=32772;
    struct trap_frame frame={.rax=LINUX_SYS_SENDMSG,.rdi=3,.rsi=(uintptr_t)memory};
    blocked=(uintptr_t)memory+32768;
    for(unsigned backing=first_backing();backing<2;++backing) {
        lazy_file=backing!=0;unmapped=0;contended=true;
        memset(memory,'a',length);memcpy(memory+32768,"tail",4);
        task_lookup_scope_begin(&current,&frame);
        struct task_user_text result={0};
        assert(task_lookup_user_bytes(&current,(uintptr_t)memory,length,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(!result.text && allocations==frees+1);
        memset(memory,'x',32768);unmapped=(uintptr_t)memory;
        task_lookup_scope_begin(&current,&frame);
        assert(task_lookup_user_bytes(&current,(uintptr_t)memory,length,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(!result.text && allocations==frees+1);
        contended=false;task_lookup_scope_begin(&current,&frame);
        assert(!task_lookup_user_bytes(&current,(uintptr_t)memory,length,&result));
        for(unsigned i=0;i<32768;++i)assert(result.text[i]=='a');
        assert(!memcmp(result.text+32768,"tail",4));
        task_lookup_scope_cancel(&current);assert(allocations==frees);
        /* The partial binary allocation has the same cancellation ownership. */
        unmapped=0;contended=true;task_lookup_scope_begin(&current,&frame);
        assert(task_lookup_user_bytes(&current,(uintptr_t)memory,length,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        task_lookup_scope_cancel(&current);task_lookup_scope_cancel(&current);assert(allocations==frees);
        contended=false;unmapped=blocked;task_lookup_scope_begin(&current,&frame);
        assert(task_lookup_user_bytes(&current,(uintptr_t)memory,length,&result)==-LINUX_EFAULT);
        task_lookup_scope_cancel(&current);assert(allocations==frees);
    }
}

int main(int argc,char **argv)
{
    assert(argc==2);current.pid=42;current.kind=TASK_KIND_USER;
    if(!strcmp(argv[1],"bytes")) {
        test_bytes();
        puts("PASS binary prefixes survive swap and cold-file contention, retries, unmap and cancellation");
        return 0;
    }
    struct vfs_namespace ns={.root=(void *)1};
    current.fs_paths.root=(struct vfs_path){.mount=(void *)1,.dentry=(void *)1};
    current.fs_paths.cwd=current.fs_paths.root;
    char *text=(char *)memory+4092;
    struct trap_frame f={.rax=!strcmp(argv[1],"path") ? LINUX_SYS_OPENAT : LINUX_SYS_EXECVE,
                         .rdi=LINUX_AT_FDCWD,.rsi=(uintptr_t)text};
    blocked=(uintptr_t)memory+4096;
    /* First model an actual swap entry; then a cold file VMA whose swap
     * preparation succeeds but page-in needs a foreign disk owner to run. */
    for(unsigned backing=first_backing();backing<2;++backing) {
        lazy_file=backing!=0;walks=0;
        memcpy(text,"/oldname",9);unmapped=0;contended=true;
        task_lookup_scope_begin(&current,&f);
        struct task_user_text result={0};struct vfs_lookup_result lookup={0};
        if(!strcmp(argv[1],"path"))
            assert(resolve_user_object_at_flags(&ns,&current,LINUX_AT_FDCWD,(uintptr_t)text,false,false,0,&lookup)==KERNEL_SYSCALL_SWAP_CONTENDED);
        else assert(task_lookup_user_text(&current,(uintptr_t)text,64,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(!walks && allocations==frees+1);
        /* A second failed attempt retains the same snapshot, without reading
         * its first page again or allocating another copy. */
        memcpy(text,"/bad",4);unmapped=(uintptr_t)memory;
        task_lookup_scope_begin(&current,&f);
        if(!strcmp(argv[1],"path"))
            assert(resolve_user_object_at_flags(&ns,&current,LINUX_AT_FDCWD,(uintptr_t)text,false,false,0,&lookup)==KERNEL_SYSCALL_SWAP_CONTENDED);
        else assert(task_lookup_user_text(&current,(uintptr_t)text,64,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(!walks && allocations==frees+1);
        contended=false;task_lookup_scope_begin(&current,&f);
        if(!strcmp(argv[1],"path")) {
            assert(!resolve_user_object_at_flags(&ns,&current,LINUX_AT_FDCWD,(uintptr_t)text,false,false,0,&lookup));
            assert(walks==1 && !strcmp(observed,"/oldname"));vfs_path_put(&lookup.path);
        } else {
            assert(!task_lookup_user_text(&current,(uintptr_t)text,64,false,&result));
            assert(!strcmp(result.text,"/oldname"));
        }
        task_lookup_scope_cancel(&current);assert(allocations==frees);
        /* Cancellation after an accepted prefix releases it exactly once. */
        memcpy(text,"/oldname",9);unmapped=0;contended=true;task_lookup_scope_begin(&current,&f);
        if(!strcmp(argv[1],"path"))
            assert(resolve_user_object_at_flags(&ns,&current,LINUX_AT_FDCWD,(uintptr_t)text,false,false,0,&lookup)==KERNEL_SYSCALL_SWAP_CONTENDED);
        else assert(task_lookup_user_text(&current,(uintptr_t)text,64,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
        task_lookup_scope_cancel(&current);task_lookup_scope_cancel(&current);assert(allocations==frees);
        /* A terminal second-page failure discards the prefix and stays EFAULT. */
        contended=false;unmapped=blocked;task_lookup_scope_begin(&current,&f);
        if(!strcmp(argv[1],"path"))
            assert(resolve_user_object_at_flags(&ns,&current,LINUX_AT_FDCWD,(uintptr_t)text,false,false,0,&lookup)==-LINUX_EFAULT);
        else assert(task_lookup_user_text(&current,(uintptr_t)text,64,false,&result)==-LINUX_EFAULT);
        task_lookup_scope_cancel(&current);assert(allocations==frees);
    }
    puts("PASS pathname/text prefixes survive swap and cold-file contention, retries, unmap and cancellation");return 0;
}
